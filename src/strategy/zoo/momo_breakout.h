#pragma once
#include "../strategy.h"
#include "zoo_common.h"
#include <cmath>
#include <string>
#include <vector>

namespace trader {

// User-supplied spec (2026-08-25), tested as given:
//
//   Long, when ALL true:
//     SMA(range, 10) > SMA(range, 50)        (volatility expanding)
//     close > close 20 bars ago
//     close > highest close of last 20 bars
//     candle is green (close > open)
//   Short = the mirror.
//   Exit: stop -1.5%, take-profit +3%.
//
// Two deviations forced by this engine, both stated rather than hidden:
//
//   * LONG HALF ONLY. The engine is long/flat and cannot short, so the mirror
//     rule is untestable here. Results below are for half the spec.
//   * EXITS CHECK THE CLOSE, NOT THE INTRABAR PATH. Strategies see bars, so
//     the -1.5%/+3% bracket is evaluated against each bar's close and filled
//     at the next open. A real bracket order exits intrabar. The bias cuts
//     both ways: close-evaluation overshoots the stop (losses larger than
//     -1.5%) and overshoots the target (wins larger than +3%), and it also
//     MISSES intrabar stop-outs on bars that dip through -1.5% but close
//     back above it. Net direction is ambiguous; on bars whose typical move
//     is comparable to the bracket (4h crypto: ~1.3% per bar) the
//     approximation is material either way, on 5m/15m bars it is small.
//
// Notes on the spec itself, recorded while implementing:
//   * "close > highest close of last 20 bars" implies "close > close 20 bars
//     ago" (that close is in the window), so condition 2 is redundant. Both
//     are implemented anyway - the test is of the spec as written.
//   * The percentage bracket is not scale-invariant: -1.5% means a different
//     number of volatility units on every instrument and timeframe, unlike
//     the ATR- and sigma-denominated exits elsewhere in this repo. Expect
//     timeframe sensitivity.
struct MomoBreakoutParams {
    int rangeFast = 10;     // SMA window of (high - low), fast
    int rangeSlow = 50;     // SMA window of (high - low), slow
    int lookback = 20;      // breakout / momentum window
    double stopPct = 1.5;   // stop loss, percent below entry fill
    double tpPct = 3.0;     // take profit, percent above entry fill
};

class MomoBreakoutStrategy : public Strategy {
public:
    explicit MomoBreakoutStrategy(MomoBreakoutParams p = {}) : p_(p) {}
    std::string name() const override { return "momo_breakout"; }

    void prepare(const CandleSeries& s) override {
        size_t n = s.size();
        std::vector<double> range(n);
        for (size_t i = 0; i < n; ++i) range[i] = s.high[i] - s.low[i];
        rangeFast_ = indicators::sma(range, std::max(2, p_.rangeFast));
        rangeSlow_ = indicators::sma(range, std::max(3, p_.rangeSlow));
        // "last 20 bars" excludes the bar being decided on: a channel that
        // includes the current close can never be broken by it.
        priorHigh_ = zoo::rollingMaxExclusive(s.close, std::max(2, p_.lookback));
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext& pos) override {
        if (pos.inPosition) {
            if (pos.entryPrice <= 0.0) return Signal::Hold;
            double ret = s.close[i] / pos.entryPrice - 1.0;
            if (ret <= -p_.stopPct / 100.0) return Signal::Sell;  // stop
            if (ret >= p_.tpPct / 100.0) return Signal::Sell;     // take profit
            return Signal::Hold;                                   // spec has no other exit
        }
        if (i < static_cast<size_t>(p_.lookback)) return Signal::Hold;
        if (!zoo::ok(rangeFast_[i]) || !zoo::ok(rangeSlow_[i]) || !zoo::ok(priorHigh_[i]))
            return Signal::Hold;
        bool volExpanding = rangeFast_[i] > rangeSlow_[i];
        bool momentum = s.close[i] > s.close[i - p_.lookback];
        bool breakout = s.close[i] > priorHigh_[i];
        bool green = s.close[i] > s.open[i];
        if (volExpanding && momentum && breakout && green) return Signal::Buy;
        return Signal::Hold;
    }

private:
    MomoBreakoutParams p_;
    std::vector<double> rangeFast_, rangeSlow_, priorHigh_;
};

} // namespace trader
