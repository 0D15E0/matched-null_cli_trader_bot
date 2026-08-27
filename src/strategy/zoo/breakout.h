#pragma once
#include "../strategy.h"
#include "zoo_common.h"
#include <cmath>
#include <string>
#include <vector>

namespace trader {

// ---------------------------------------------------------------------------
// Donchian channel breakout - the rule the Turtles were taught (Donchian's
// 4-week rule; Faith, "Way of the Turtles"). Buy when price makes a new
// N-bar high, exit on a new M-bar low or an ATR trailing stop, whichever
// comes first.
//
// Two details this implementation is deliberate about:
//   * the channel excludes the current bar. A channel that includes today's
//     own high can never be broken by today's close, so the "breakout" would
//     silently become a comparison against a bound that already moved.
//   * the stop is a multiple of ATR, not a percentage. A 5% stop is loose on
//     SHY and suicidal on DOGE; k * ATR is the same statement about how far
//     price has to move against you, everywhere.
// ---------------------------------------------------------------------------
struct DonchianParams {
    int entryWindow = 55;
    int exitWindow = 20;
    int atrWindow = 20;
    double atrStopMult = 2.5;   // trailing stop distance below the high-water close; 0 = off
};

class DonchianStrategy : public Strategy {
public:
    explicit DonchianStrategy(DonchianParams p = {}) : p_(p) {}
    std::string name() const override { return "donchian"; }

    void prepare(const CandleSeries& s) override {
        upper_ = zoo::rollingMaxExclusive(s.high, std::max(2, p_.entryWindow));
        lower_ = zoo::rollingMinExclusive(s.low, std::max(2, p_.exitWindow));
        atr_ = indicators::atr(s.high, s.low, s.close, std::max(2, p_.atrWindow));
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext& pos) override {
        if (i >= upper_.size()) return Signal::Hold;
        if (pos.inPosition) {
            if (zoo::ok(lower_[i]) && s.close[i] < lower_[i]) return Signal::Sell;
            if (p_.atrStopMult > 0.0 && zoo::ok(atr_[i])) {
                double stop = pos.highestClose - p_.atrStopMult * atr_[i];
                if (s.close[i] < stop) return Signal::Sell;
            }
            return Signal::Hold;
        }
        if (zoo::ok(upper_[i]) && s.close[i] > upper_[i]) return Signal::Buy;
        return Signal::Hold;
    }

private:
    DonchianParams p_;
    std::vector<double> upper_, lower_, atr_;
};

// ---------------------------------------------------------------------------
// Volatility-contraction breakout ("squeeze"). Bollinger BandWidth
// - (upper - lower) / mid - is a scale-free measure of how tightly the recent
// distribution of prices is packed. Long periods of contraction are followed
// by expansion far more reliably than the direction of that expansion can be
// predicted, which is the empirical content behind volatility clustering
// (Engle 1982) read as a trading rule rather than as a risk model.
//
// The rule: when bandwidth sits in the bottom `squeezePct` of its own
// trailing distribution, arm; while armed, take the first close above the
// upper band; exit on a close back below the middle band.
//
// The arming threshold is a PERCENTILE of the instrument's own history, not
// an absolute bandwidth, for the same reason every other threshold in this
// zoo is relative: absolute bandwidth encodes the instrument.
// ---------------------------------------------------------------------------
struct SqueezeParams {
    int window = 20;
    double numSigma = 2.0;
    int rankWindow = 250;      // history the percentile is measured against
    double squeezePct = 0.20;  // arm when bandwidth rank is below this
    int armBars = 10;          // how long an arming stays live
};

class SqueezeBreakoutStrategy : public Strategy {
public:
    explicit SqueezeBreakoutStrategy(SqueezeParams p = {}) : p_(p) {}
    std::string name() const override { return "squeeze_breakout"; }

    void prepare(const CandleSeries& s) override {
        int w = std::max(5, p_.window);
        bands_ = indicators::bollinger(s.close, w, p_.numSigma);
        size_t n = s.close.size();
        std::vector<double> width(n, std::nan(""));
        for (size_t i = 0; i < n; ++i)
            if (zoo::ok(bands_.mid[i]) && bands_.mid[i] > 0.0)
                width[i] = (bands_.upper[i] - bands_.lower[i]) / bands_.mid[i];
        auto rank = zoo::rollingPercentileRank(width, std::max(20, p_.rankWindow));

        // Arming is a pure function of price history, so it is precomputed
        // here rather than latched inside onBar - the Strategy contract
        // requires re-evaluating a bar to give the same answer, and a latch
        // mutated during evaluation would break replay in the live loop.
        armed_.assign(n, false);
        int countdown = 0;
        for (size_t i = 0; i < n; ++i) {
            if (zoo::ok(rank[i]) && rank[i] <= p_.squeezePct) countdown = std::max(1, p_.armBars);
            armed_[i] = countdown > 0;
            if (countdown > 0) --countdown;
        }
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext& pos) override {
        if (i >= armed_.size() || !zoo::ok(bands_.mid[i])) return Signal::Hold;
        if (pos.inPosition) return s.close[i] < bands_.mid[i] ? Signal::Sell : Signal::Hold;
        if (armed_[i] && s.close[i] > bands_.upper[i]) return Signal::Buy;
        return Signal::Hold;
    }

private:
    SqueezeParams p_;
    indicators::BollingerResult bands_;
    std::vector<char> armed_;
};

} // namespace trader
