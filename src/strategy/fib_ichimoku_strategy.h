#pragma once
#include "strategy.h"
#include "../indicators/indicators.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace trader {

// Fibonacci retracement entries inside an Ichimoku-confirmed uptrend.
//
// THE IDEA, AS SPECIFIED
// ----------------------
// Detect a clear completed up-swing - a confirmed bottom followed by a
// confirmed top, Darvas-box style - and treat that swing's range as the frame
// of reference. With the swing top at 0 and the swing bottom at 1:
//
//     0.500 / 0.618 retracement   the pullback zone, where a long is entered
//     1.000                       the swing bottom; below it the setup is dead
//    -0.272 / -0.414 / -0.618     extensions ABOVE the top: profit targets
//
// Ichimoku supplies the regime - only take the pullback when the cloud says the
// trend is up - and Fibonacci supplies the timing and the targets. This is a
// DEFINED-RISK, DEFINED-TARGET trade, which makes it a genuinely different
// animal from every continuous trend rule in this repo: the payoff is a bounded
// win against a bounded loss rather than an open-ended ride.
//
// THE LOOK-AHEAD TRAP THIS AVOIDS
// -------------------------------
// A pivot high at bar j is only a pivot once you have seen `pivotWindow` bars
// AFTER j without a higher high. Almost every published Fibonacci or
// swing-based backtest gets this wrong: it locates swings on the completed
// series and then "enters" at a retracement of a swing whose top had not yet
// happened at entry time - which reads the future and produces beautiful,
// meaningless equity curves. Here a pivot at j is usable only from bar
// j + pivotWindow onward, so every level the strategy trades against was
// computable at the moment it acted. `confirmBars()` exposes the lag so a test
// can assert it.
//
// WHAT TO EXPECT
// --------------
// Fibonacci ratios have no accepted theoretical basis in price formation and
// the published evidence for them is weak. The honest reasons to test this are
// that its payoff SHAPE is new here, that the swing structure is objective once
// pivots are defined, and that its parameters are mostly pinned by the
// specification rather than free. The ratios below are fixed constants, not
// fitted values - if this works, it is not because those numbers were tuned.
struct FibIchimokuParams {
    // Swing detection.
    int    pivotWindow = 10;      // bars either side that define a pivot
    double minSwingAtr = 4.0;     // a swing must span this many ATRs to count
                                  // as "clear"; ATR-relative so the same
                                  // setting means the same thing on 1h and 1d
    int    atrWindow = 14;
    int    maxSwingAgeBars = 200; // a swing this old is stale, not a setup

    // Entry zone, as retracement fractions of the swing (0 = top, 1 = bottom).
    double entryZoneNear = 0.500;
    double entryZoneFar  = 0.618;

    // Exit. Negative fractions are extensions above the swing top.
    double targetLevel = -0.272;  // -0.272 / -0.414 / -0.618
    double stopLevel   = 1.050;   // just under the swing bottom

    // Ichimoku regime filter. 0 = none, 1 = trend (not below cloud, bullish
    // cloud), 2 = full (adds tenkan>kijun and the chikou confirmation).
    int    ichimokuStrictness = 1;
    int    tenkanWindow = 9, kijunWindow = 26, spanBWindow = 52;

    bool   exitOnTrendBreak = true; // leave if price closes below the cloud
};

class FibIchimokuStrategy : public Strategy {
public:
    using Params = FibIchimokuParams;

    explicit FibIchimokuStrategy(Params p = {}) : params_(p) {}

    std::string name() const override { return "fib_ichimoku"; }

    // Bars between a pivot forming and the strategy being allowed to use it.
    int confirmBars() const { return params_.pivotWindow; }

    void prepare(const CandleSeries& series) override {
        const size_t n = series.size();
        ichi_ = indicators::ichimoku(series.high, series.low, params_.tenkanWindow,
                                      params_.kijunWindow, params_.spanBWindow);
        atr_ = indicators::atr(series.high, series.low, series.close, params_.atrWindow);

        // Mark pivots. A pivot at j is strictly interior to its window, so the
        // scan stops `k` short of both ends.
        const int k = std::max(1, params_.pivotWindow);
        pivotHigh_.assign(n, false);
        pivotLow_.assign(n, false);
        for (size_t j = static_cast<size_t>(k); j + static_cast<size_t>(k) < n; ++j) {
            bool hi = true, lo = true;
            for (int d = -k; d <= k; ++d) {
                if (d == 0) continue;
                size_t m = j + static_cast<size_t>(d);
                if (series.high[m] >= series.high[j]) hi = false;
                if (series.low[m] <= series.low[j]) lo = false;
            }
            pivotHigh_[j] = hi;
            pivotLow_[j] = lo;
        }
        activeTop_ = activeBot_ = std::nan("");
        activeTopIdx_ = 0;
        entryTop_ = entryBot_ = std::nan("");
    }

    Signal onBar(const CandleSeries& series, size_t i, const PositionContext& pos) override {
        const int k = std::max(1, params_.pivotWindow);
        if (i < static_cast<size_t>(k) + 2) return Signal::Hold;

        // Roll the newest CONFIRMED pivot into the active swing. Bar i can only
        // see pivots at index <= i - k.
        const size_t newlyConfirmed = i - static_cast<size_t>(k);
        if (pivotLow_[newlyConfirmed]) {
            // A fresh low resets the frame: the swing being measured is always
            // the most recent bottom-then-top pair.
            activeBot_ = series.low[newlyConfirmed];
            activeTop_ = std::nan("");
        } else if (pivotHigh_[newlyConfirmed] && std::isfinite(activeBot_)) {
            double h = series.high[newlyConfirmed];
            if (h > activeBot_) { activeTop_ = h; activeTopIdx_ = newlyConfirmed; }
        }

        const double close = series.close[i];

        if (pos.inPosition) {
            // Targets and stops are evaluated against the swing that was live
            // at ENTRY, not against whatever swing has formed since - the trade
            // has to be judged on the frame it was taken in.
            if (std::isfinite(entryTop_) && std::isfinite(entryBot_)) {
                double range = entryTop_ - entryBot_;
                double target = entryTop_ - params_.targetLevel * range;  // negative -> above top
                double stop = entryTop_ - params_.stopLevel * range;
                if (close >= target || close <= stop) return Signal::Sell;
            }
            if (params_.exitOnTrendBreak && trendBrokenAt(series, i)) return Signal::Sell;
            return Signal::Hold;
        }

        // Need a complete, clear, fresh swing.
        if (!std::isfinite(activeTop_) || !std::isfinite(activeBot_)) return Signal::Hold;
        double range = activeTop_ - activeBot_;
        if (!(range > 0.0)) return Signal::Hold;
        if (!std::isfinite(atr_[i]) || atr_[i] <= 0.0) return Signal::Hold;
        if (range < params_.minSwingAtr * atr_[i]) return Signal::Hold;   // not "clear"
        if (i > activeTopIdx_ + static_cast<size_t>(params_.maxSwingAgeBars)) return Signal::Hold;

        // Dead if price has already broken the swing bottom.
        if (close <= activeBot_) { activeTop_ = std::nan(""); return Signal::Hold; }

        // Has price REACHED the retracement level?
        //
        // Entry semantics matter more than they look. Requiring the CLOSE to
        // land inside the [near, far] band is how this was first written, and
        // on daily bars a close almost never lands inside a band a few percent
        // of the swing wide - the strategy took ZERO trades and reported it as
        // a flat result. A trader placing a resting bid at 0.618 is filled when
        // price TRADES THERE, so the trigger is the bar's LOW reaching the
        // level, not its close sitting in a window.
        //
        // The trigger is FIRST CONTACT with the zone's shallow edge
        // (entryZoneNear, e.g. 0.500), which is what a resting bid at that
        // level actually does: once price trades there you are filled, and if
        // it keeps falling toward entryZoneFar (0.618) you are already long
        // with the stop below the swing low to handle it.
        //
        // The earlier version demanded the low reach the DEEP edge *and* the
        // close sit under the shallow one. That is a much stricter rule than
        // "enter in the 0.5-0.618 zone" and it starved the strategy: on daily
        // bars it produced zero trades, which the report then presented as a
        // flat result rather than as a rule that never fires.
        double near = activeTop_ - params_.entryZoneNear * range;
        double far = activeTop_ - params_.entryZoneFar * range;
        (void)far;   // retained for the stop/target geometry and validation
        bool reached = series.low[i] <= near;
        if (!reached) return Signal::Hold;

        if (!ichimokuAgrees(series, i)) return Signal::Hold;

        entryTop_ = activeTop_;
        entryBot_ = activeBot_;
        return Signal::Buy;
    }

private:
    // Trend invalidation: a close below the cloud says the frame the trade was
    // taken in no longer holds, whatever the fib levels say.
    bool trendBrokenAt(const CandleSeries& series, size_t i) const {
        double a = ichi_.spanA[i], b = ichi_.spanB[i];
        if (std::isnan(a) || std::isnan(b)) return false;
        return series.close[i] < std::min(a, b);
    }

    bool ichimokuAgrees(const CandleSeries& series, size_t i) const {
        if (params_.ichimokuStrictness <= 0) return true;
        double a = ichi_.spanA[i], b = ichi_.spanB[i];
        double t = ichi_.tenkan[i], kj = ichi_.kijun[i];
        if (std::isnan(a) || std::isnan(b) || std::isnan(t) || std::isnan(kj)) return false;
        double kumoBot = std::min(a, b);
        double close = series.close[i];

        // Level 1 - the trend must be up: price is not below the cloud, and the
        // cloud itself is bullish. A pullback entry deliberately does NOT
        // require price above the cloud top, because the retracement is often
        // exactly a tag of the cloud.
        if (close < kumoBot) return false;
        if (a <= b) return false;
        if (params_.ichimokuStrictness == 1) return true;

        // Level 2 - full: momentum still up, and the lagging span confirms.
        if (t <= kj) return false;
        size_t lag = static_cast<size_t>(params_.kijunWindow);
        if (i < lag) return false;
        if (series.close[i] <= series.close[i - lag]) return false;  // chikou above past price
        return true;
    }

    Params params_;
    indicators::IchimokuResult ichi_;
    std::vector<double> atr_;
    std::vector<bool> pivotHigh_, pivotLow_;
    double activeTop_ = 0.0, activeBot_ = 0.0;
    size_t activeTopIdx_ = 0;
    double entryTop_ = 0.0, entryBot_ = 0.0;
};

} // namespace trader
