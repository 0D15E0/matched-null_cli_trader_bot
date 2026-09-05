#pragma once
#include "../strategy.h"
#include "../../indicators/indicators.h"
#include "zoo_common.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace trader {

// Ichimoku Kinko Hyo, the COMPLETE classic system - the repo's existing
// PureIchimokuStrategy implements cloud + TK cross but omits the third leg,
// the Chikou span, which the traditional rule treats as mandatory
// confirmation.
//
// Chikou is the close plotted `displacement` bars BEHIND. The textbook
// condition "Chikou is above the price" evaluated at bar i is simply
// close[i] > close[i - displacement]: no future data, despite the lagging
// plot. (The Senkou spans are the mirror trap - they are drawn 26 bars AHEAD,
// so the cloud read at bar i must be built from bar i-26. indicators.cpp
// already does this correctly, which is why this file only has to add Chikou.)
//
// Parameters are the published 9/26/52/26 and are NOT swept: the point of
// testing a named system is to test SOMEONE ELSE'S choices out of sample. A
// tuned Ichimoku would just be another entry in the 53,000-candidate graveyard.
struct IchimokuFullParams {
    int tenkan = 9;
    int kijun = 26;
    int spanB = 52;
    int displacement = 26;
    int requireChikou = 1;   // 0 reproduces the existing pure_ichimoku rule
};

class IchimokuFullStrategy : public Strategy {
public:
    explicit IchimokuFullStrategy(IchimokuFullParams p = {}) : p_(p) {}
    std::string name() const override { return "ichimoku_full"; }

    void prepare(const CandleSeries& s) override {
        ichi_ = indicators::ichimoku(s.high, s.low, p_.tenkan, p_.kijun, p_.spanB, p_.displacement);
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext& pos) override {
        if (i == 0) return Signal::Hold;
        if (std::isnan(ichi_.spanA[i]) || std::isnan(ichi_.spanB[i]) ||
            std::isnan(ichi_.tenkan[i]) || std::isnan(ichi_.kijun[i]))
            return Signal::Hold;

        const double close = s.close[i];
        const double top = std::max(ichi_.spanA[i], ichi_.spanB[i]);
        const double bot = std::min(ichi_.spanA[i], ichi_.spanB[i]);
        const bool tkBull = ichi_.tenkan[i] > ichi_.kijun[i];

        // Chikou: today's close against the close `displacement` bars ago.
        bool chikouBull = true;
        if (p_.requireChikou) {
            if (i < static_cast<size_t>(p_.displacement)) return Signal::Hold;
            chikouBull = close > s.close[i - static_cast<size_t>(p_.displacement)];
        }

        if (pos.inPosition) {
            if (close < bot || !tkBull) return Signal::Sell;
            return Signal::Hold;
        }
        if (close > top && tkBull && chikouBull) return Signal::Buy;
        return Signal::Hold;
    }

private:
    IchimokuFullParams p_;
    indicators::IchimokuResult ichi_;
};

} // namespace trader
