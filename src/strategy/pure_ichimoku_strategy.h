#pragma once
#include "strategy.h"
#include "../indicators/indicators.h"
#include <cmath>
#include <algorithm>

namespace trader {

// "Pure" Ichimoku Kinko Hyo strategy - no DMI/ADX trend-strength filter,
// no ATR trailing stop. Included as a baseline to compare against
// OdiseoStrategy (which layers DMI/ADX confirmation + an ATR trailing
// stop on top of the same Ichimoku cloud signal) so the value of those
// extra filters can be measured directly rather than assumed.
//
// Entry:  close crosses above the kumo (cloud) AND tenkan > kijun
//         (classic "strong bullish" Ichimoku signal).
// Exit:   close crosses back below the kumo, OR tenkan crosses back
//         below kijun (whichever comes first) - no ATR stop.
class PureIchimokuStrategy : public Strategy {
public:
    explicit PureIchimokuStrategy(int tenkanWindow = 9, int kijunWindow = 26, int spanBWindow = 52)
        : tenkanWindow_(tenkanWindow), kijunWindow_(kijunWindow), spanBWindow_(spanBWindow) {}

    std::string name() const override { return "pure_ichimoku"; }

    void prepare(const CandleSeries& series) override {
        ichi_ = indicators::ichimoku(series.high, series.low, tenkanWindow_, kijunWindow_, spanBWindow_);
    }

    Signal onBar(const CandleSeries& series, size_t i, const PositionContext& pos) override {
        if (i == 0) return Signal::Hold;
        if (std::isnan(ichi_.spanA[i]) || std::isnan(ichi_.spanB[i]) ||
            std::isnan(ichi_.tenkan[i]) || std::isnan(ichi_.kijun[i]))
            return Signal::Hold;

        double close = series.close[i];
        double kumoTop = std::max(ichi_.spanA[i], ichi_.spanB[i]);
        double kumoBot = std::min(ichi_.spanA[i], ichi_.spanB[i]);
        bool aboveCloud = close > kumoTop;
        bool belowCloud = close < kumoBot;
        bool tenkanAboveKijun = ichi_.tenkan[i] > ichi_.kijun[i];

        if (pos.inPosition) {
            if (belowCloud || !tenkanAboveKijun) return Signal::Sell;
            return Signal::Hold;
        }

        if (aboveCloud && tenkanAboveKijun) return Signal::Buy;
        return Signal::Hold;
    }

private:
    int tenkanWindow_, kijunWindow_, spanBWindow_;
    indicators::IchimokuResult ichi_;
};

} // namespace trader
