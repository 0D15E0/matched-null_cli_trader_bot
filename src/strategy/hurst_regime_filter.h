#pragma once
#include "strategy.h"
#include "../indicators/indicators.h"
#include <memory>
#include <cmath>

namespace trader {

// Decorator that gates a wrapped trend-following strategy's Buy/Sell
// signals behind a rolling Hurst exponent regime check.
//
// The Hurst exponent (rescaled-range analysis; see indicators::hurstExponent)
// estimates whether recent price action is persistent/trending (H > 0.5),
// a pure random walk (H ~= 0.5), or mean-reverting/anti-persistent
// (H < 0.5). Trend-following strategies like `odiseo`/`pure_ichimoku`/
// `tsmom` are expected to perform best when H is meaningfully above 0.5,
// and to whipsaw/underperform in random-walk or mean-reverting regimes.
// This is a well-established idea in the fractal-market-hypothesis
// literature (Hurst 1951; Mandelbrot; Peters, "Fractal Market Analysis")
// and still shows up as a regime-detection pre-filter in modern
// regime-aware trading research.
//
// Buy signals from the wrapped strategy are only forwarded when
// H(i) >= trendThreshold; otherwise they're suppressed to Hold. Sells are
// never suppressed, mirroring odiseo/pure_ichimoku's own philosophy of
// "entries are filtered, exits are not".
//
// This decorator used to be unsound: it called the inner strategy first and
// then discarded a Buy, but the inner strategy had already flipped its own
// `inPosition_` flag, so it believed it held a position the engine had never
// opened. It then suppressed every genuine later entry until a phantom
// trailing stop fired against a price nothing was bought at - meaning every
// "+hurst_filter" comparison measured quasi-random whole-trade dropout
// rather than regime filtering. Strategies are now stateless with respect to
// position (see strategy.h's PositionContext), so suppressing a signal here
// has no side effect on the inner strategy at all, and the A/B comparison
// finally measures what it claims to.
//
// Note also that indicators::hurstExponent is now bias-corrected: the raw
// R/S estimator it used to call reported H >= 0.55 on more than half of pure
// random-walk windows, so this gate was close to a coin flip. Thresholds
// tuned against the old estimator do not carry over.
// Which Hurst estimator backs the gate.
//
//   RescaledRange    - the classic R/S estimator (indicators::hurstExponent).
//   StructureFunction - the q=2 structure-function slope, the default.
//
// Measured side by side on fractional Brownian motion with known H, at the
// ROLLING window lengths this filter actually uses (not the long series where
// both look good), the structure function is the better instrument - though by
// less than a long-sample comparison suggests:
//
//   true H    structure fn (bias, sd)     R/S (bias, sd)      [window=200]
//   0.5       -0.029, 0.073               -0.032, 0.100
//   0.6       -0.017, 0.080               -0.034, 0.121
//   0.7       -0.026, 0.078               -0.075, 0.123
//
// The gap that matters is at HIGH H: R/S under-reads persistent regimes by
// ~0.08 at H=0.7, so a gate at 0.55 rejects genuinely trending windows that
// the structure function correctly passes. Its spread is also ~25% tighter,
// and its false-positive rate on a true random walk at window=200 is 21.8%
// against R/S's 29.6%.
//
// Both estimators still pass ~1/4 of random-walk windows at a 0.55 gate. This
// is a weak instrument either way - short-window Hurst estimation is hard, and
// no amount of estimator choice fixes that. Treat the threshold as a tuning
// parameter to be validated, not as a physical constant.
enum class HurstEstimator { RescaledRange, StructureFunction };

class HurstRegimeFilter : public Strategy {
public:
    HurstRegimeFilter(std::unique_ptr<Strategy> inner, int hurstWindow = 100,
                       double trendThreshold = 0.55,
                       HurstEstimator estimator = HurstEstimator::StructureFunction)
        : inner_(std::move(inner)), hurstWindow_(hurstWindow),
          trendThreshold_(trendThreshold), estimator_(estimator) {}

    std::string name() const override {
        return inner_->name() + "+hurst_filter" +
               (estimator_ == HurstEstimator::RescaledRange ? "(rs)" : "(sf)");
    }

    void prepare(const CandleSeries& series) override {
        inner_->prepare(series);
        hurst_ = estimator_ == HurstEstimator::StructureFunction
                     ? indicators::hurstStructureFunction(series.close, hurstWindow_)
                     : indicators::hurstExponent(series.close, hurstWindow_);
    }

    Signal onBar(const CandleSeries& series, size_t i, const PositionContext& pos) override {
        Signal signal = inner_->onBar(series, i, pos);
        if (signal != Signal::Buy) return signal; // exits and holds pass through

        // If we don't have a Hurst estimate yet (warm-up period), fall back
        // to trusting the inner strategy rather than blocking it entirely.
        if (i >= hurst_.size() || std::isnan(hurst_[i])) return signal;

        return hurst_[i] >= trendThreshold_ ? Signal::Buy : Signal::Hold;
    }

private:
    std::unique_ptr<Strategy> inner_;
    int hurstWindow_;
    double trendThreshold_;
    HurstEstimator estimator_;
    std::vector<double> hurst_;
};

} // namespace trader
