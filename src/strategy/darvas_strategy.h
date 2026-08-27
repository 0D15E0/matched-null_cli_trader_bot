#pragma once
#include "strategy.h"
#include "../indicators/indicators.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace trader {

// Darvas box breakout, volume-confirmed, with Fibonacci extension targets.
//
// Nicolas Darvas's method, made objective. Three structures, each doing the job
// it is actually good at:
//
//   IMPULSE SWING  a confirmed pivot low followed by a confirmed pivot high.
//                  Supplies the RANGE that the profit targets are measured
//                  from, because the size of the move that got price here is
//                  the honest scale for how far the next leg might run.
//   BOX            after the impulse high, price consolidates: box top is the
//                  impulse high, box bottom is the lowest low since. Supplies
//                  the ENTRY (a close above the top) and the STOP (the bottom).
//   VOLUME         a breakout on ordinary volume is not a breakout. Entry
//                  additionally requires relative volume >= volMult.
//
// WHY THE TARGET USES THE SWING AND NOT THE BOX. Measuring the Fibonacci
// extension from the box height instead is the obvious-looking choice and it is
// arithmetically bad: the stop is the box height (that is what you risk), so a
// target of 0.618 x box height is 0.62:1 reward-to-risk and needs a 62% hit
// rate to break even. Measuring from the impulse range keeps the stop tiny (the
// box is a consolidation) while the target scales with the move that preceded
// it. That asymmetry IS the strategy; without it there is nothing here.
//
// WHY THIS DIRECTION AND NOT A RETRACEMENT ENTRY. A Fibonacci retracement entry
// (see fib_ichimoku_strategy.h) buys after a trend has given back 50-62% of its
// move, and measured on this repo's data it costs 0.45 Sharpe on average - it
// systematically selects the setups that are failing. A box breakout buys
// strength instead. In an asset class where trend continuation is the one
// measured effect, that is betting with the grain rather than against it.
//
// CAUSALITY. A pivot at bar j is only usable from bar j + pivotWindow, because
// that is when "nothing exceeded it" becomes knowable. Box bottoms, volume and
// the breakout test all read bars <= i. Relative volume compares bar i against
// the mean of the bars BEFORE it, so a single huge bar cannot raise its own
// baseline.
struct DarvasParams {
    // Structure detection.
    int    pivotWindow = 10;      // bars either side that define a pivot
    int    minBoxBars = 5;        // consolidation must last this long
    int    maxBoxBars = 120;      // after this the box is stale, not a setup
    double maxBoxWidthFrac = 0.25;// box height / box top; wider is a downtrend,
                                  // not a consolidation
    double minSwingAtr = 3.0;     // impulse must span this many ATRs to count
    int    atrWindow = 14;

    // Volume confirmation.
    bool   requireVolume = true;
    int    volWindow = 20;
    double volMult = 1.3;         // breakout bar volume vs recent normal

    // Exit. Target is impulseTop + targetLevel * impulseRange, so the
    // specified 0.272 / 0.414 / 0.618 are the extensions above the top.
    double targetLevel = 0.618;
    double stopBelowBoxFrac = 0.0;// extra cushion under the box bottom, as a
                                  // fraction of box height
    bool   trailToNewBox = true;  // Darvas's own rule: as higher boxes form,
                                  // raise the stop to the new box bottom
};

class DarvasStrategy : public Strategy {
public:
    using Params = DarvasParams;
    explicit DarvasStrategy(Params p = {}) : params_(p) {}
    std::string name() const override { return "darvas"; }
    int confirmBars() const { return params_.pivotWindow; }

    void prepare(const CandleSeries& series) override {
        const size_t n = series.size();
        atr_ = indicators::atr(series.high, series.low, series.close, params_.atrWindow);
        relVol_ = indicators::relativeVolume(series.volume, params_.volWindow);
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
        swingLow_ = swingHigh_ = std::nan("");
        boxTop_ = boxBot_ = std::nan("");
        boxStart_ = 0;
        entryTarget_ = entryStop_ = std::nan("");
    }

    Signal onBar(const CandleSeries& series, size_t i, const PositionContext& pos) override {
        const int k = std::max(1, params_.pivotWindow);
        if (i < static_cast<size_t>(k) + 2) return Signal::Hold;
        const size_t confirmed = i - static_cast<size_t>(k);

        // Track the impulse swing, and open a box when its high is confirmed.
        if (pivotLow_[confirmed]) {
            swingLow_ = series.low[confirmed];
            swingHigh_ = std::nan("");
            boxTop_ = boxBot_ = std::nan("");
        } else if (pivotHigh_[confirmed] && std::isfinite(swingLow_)) {
            double h = series.high[confirmed];
            if (h > swingLow_) {
                swingHigh_ = h;
                boxTop_ = h;                 // the box ceiling IS the impulse high
                boxBot_ = series.low[confirmed];
                boxStart_ = confirmed;
            }
        }

        // Extend the box bottom while price stays under the ceiling.
        if (std::isfinite(boxTop_)) {
            if (series.low[i] < boxBot_) boxBot_ = series.low[i];
        }

        if (pos.inPosition) {
            if (std::isfinite(entryTarget_) && series.close[i] >= entryTarget_) return Signal::Sell;
            if (std::isfinite(entryStop_) && series.close[i] <= entryStop_) return Signal::Sell;
            // Darvas trailed his stop up as each higher box formed. Raising it
            // to a NEW box bottom only ever tightens risk; it never loosens.
            if (params_.trailToNewBox && std::isfinite(boxBot_) && boxBot_ > entryStop_)
                entryStop_ = boxBot_;
            return Signal::Hold;
        }

        if (!std::isfinite(boxTop_) || !std::isfinite(boxBot_) || !std::isfinite(swingLow_))
            return Signal::Hold;
        double boxH = boxTop_ - boxBot_;
        double swingR = swingHigh_ - swingLow_;
        if (!(boxH > 0.0) || !(swingR > 0.0)) return Signal::Hold;
        if (!std::isfinite(atr_[i]) || atr_[i] <= 0.0) return Signal::Hold;
        if (swingR < params_.minSwingAtr * atr_[i]) return Signal::Hold;

        size_t age = i - boxStart_;
        if (age < static_cast<size_t>(params_.minBoxBars)) return Signal::Hold;
        if (age > static_cast<size_t>(params_.maxBoxBars)) return Signal::Hold;
        if (boxH / boxTop_ > params_.maxBoxWidthFrac) return Signal::Hold;

        // The breakout itself, on volume.
        if (!(series.close[i] > boxTop_)) return Signal::Hold;
        if (params_.requireVolume) {
            if (relVol_.empty() || !std::isfinite(relVol_[i])) return Signal::Hold;
            if (relVol_[i] < params_.volMult) return Signal::Hold;
        }

        entryTarget_ = swingHigh_ + params_.targetLevel * swingR;
        entryStop_ = boxBot_ - params_.stopBelowBoxFrac * boxH;
        return Signal::Buy;
    }

private:
    Params params_;
    std::vector<double> atr_, relVol_;
    std::vector<bool> pivotHigh_, pivotLow_;
    double swingLow_ = 0.0, swingHigh_ = 0.0;
    double boxTop_ = 0.0, boxBot_ = 0.0;
    size_t boxStart_ = 0;
    double entryTarget_ = 0.0, entryStop_ = 0.0;
};

} // namespace trader
