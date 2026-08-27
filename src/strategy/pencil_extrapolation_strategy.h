#pragma once
#include "strategy.h"
#include "../math/pencil.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace trader {

// Matrix-pencil (Prony) extrapolation of log price: fit a finite sum of
// complex exponentials to the last W bars, continue it h bars forward, and
// trade the sign of the predicted move.
//
// WHAT THIS IS, AND WHAT IT IS NOT
// --------------------------------
// The pencil models a signal as
//     y[j] = sum_m c_m z_m^j ,   z_m = exp(omega_m),
// i.e. a sum of exponentially damped/growing sinusoids. |z_m| is a growth or
// decay rate and arg(z_m) is a frequency, so the fit IS a decomposition of the
// window into repeating shapes plus trends, and the model extends to j > W by
// construction. That is a genuine, classical extrapolator (Prony 1795; Hua &
// Sarkar 1990), and it is what "find the shape and continue it" means
// concretely.
//
// It is NOT the same thing as the fractional ORDER alpha that
// `order-spectrum` reports, and the difference matters enough to state twice.
// That alpha is measured from the AUTOCORRELATION of |log return|. An
// autocorrelation is the inverse transform of a POWER spectrum, and a power
// spectrum has thrown away every phase. By Wiener-Khinchin, infinitely many
// completely different price paths share one autocorrelation exactly - which
// is not a conjecture here but the working principle of the IAAFT surrogates
// used elsewhere in this project, which manufacture such paths on purpose. So
// alpha constrains how volatility CLUSTERS; it cannot be inverted to a price
// path, and no amount of estimator quality changes that. Measured directly:
// `order-spectrum --input returns` on BTC returns NOT identified, i.e. the
// estimator finds no resolvable order in the direction signal at all, while it
// identifies one in the volatility signal at alpha = -0.741.
//
// So this strategy tests the OTHER hypothesis - that the price window itself
// (not its autocorrelation) contains recurring damped-oscillatory structure
// worth extrapolating. The pencil is the right instrument for that question
// and this is an honest test of it.
//
// PRIOR: it should not work. If it did, the log price would be forecastable
// from its own past by a linear model, and 60 years of literature says it is
// not. The value of running it is a measured answer with a control attached,
// not a hope.
//
// CAUSALITY. onBar(i) reads close[0..i] only. The fit window ends at bar i and
// the prediction is for bar i+horizon, so nothing after the decision bar is
// touched. Signals are still filled at the NEXT bar's open by the engine.
//
// NUMERICS. Extrapolating a mode with |z| > 1 grows like |z|^h and a fit to
// noisy data routinely produces such modes; over long horizons they explode
// and produce a garbage signal that looks like enormous confidence. Two
// guards: modes are dropped when |z|^horizon exceeds `maxModeGrowth`, and a
// predicted move larger than `maxPredictedMove` is treated as a failed fit
// (Hold) rather than a strong signal.
struct PencilExtrapolationParams {
    int    window = 256;         // bars fitted
    int    modelOrder = 6;       // M; the dominant knob (see pencil.h)
    int    horizon = 24;         // bars ahead to predict
    int    refitEvery = 8;       // refit cadence; a fit costs an SVD
    double entryThreshold = 0.01; // |predicted log move| needed to act
    double maxModeGrowth = 50.0;  // reject modes with |z|^horizon above this
    double maxPredictedMove = 0.5; // |predicted log move| above this = bad fit
    bool   detrend = true;        // remove the window's linear trend first
};

class PencilExtrapolationStrategy : public Strategy {
public:
    using Params = PencilExtrapolationParams;

    explicit PencilExtrapolationStrategy(Params p = {}) : params_(p) {}

    std::string name() const override { return "pencil_extrap"; }

    void prepare(const CandleSeries& series) override {
        logClose_.assign(series.size(), std::nan(""));
        for (size_t i = 0; i < series.size(); ++i)
            if (series.close[i] > 0.0) logClose_[i] = std::log(series.close[i]);
        predicted_.assign(series.size(), std::nan(""));
        lastFit_ = static_cast<size_t>(-1);
        cachedSignal_ = 0.0;
    }

    // Predicted log-price change over `horizon` bars, from a fit to the window
    // ending at bar i. Returns NaN when the fit fails or is rejected. Exposed
    // so a diagnostic can score the forecast directly instead of only through
    // trading P&L.
    double predictedMove(size_t i) const {
        const int W = params_.window;
        if (i + 1 < static_cast<size_t>(W)) return std::nan("");
        std::vector<double> y;
        y.reserve(static_cast<size_t>(W));
        for (size_t k = i + 1 - static_cast<size_t>(W); k <= i; ++k) {
            if (!std::isfinite(logClose_[k])) return std::nan("");
            y.push_back(logClose_[k]);
        }

        // Detrend: a log price is dominated by drift, and a pencil spends its
        // model order fitting that ramp with near-unit modes instead of on the
        // oscillatory structure this is meant to find. Removing a straight
        // line first, then adding it back to the forecast, keeps the trend in
        // the prediction while leaving the modes to describe the shape.
        double a = 0.0, b = 0.0;
        if (params_.detrend) {
            double n = static_cast<double>(W);
            double sx = n * (n - 1.0) / 2.0, sxx = (n - 1.0) * n * (2.0 * n - 1.0) / 6.0;
            double sy = 0.0, sxy = 0.0;
            for (int j = 0; j < W; ++j) { sy += y[j]; sxy += j * y[j]; }
            double den = n * sxx - sx * sx;
            if (den == 0.0) return std::nan("");
            b = (n * sxy - sx * sy) / den;
            a = (sy - b * sx) / n;
            for (int j = 0; j < W; ++j) y[j] -= a + b * j;
        }

        mathx::PencilOptions opts;
        opts.modelOrder = params_.modelOrder;
        auto modes = mathx::matrixPencil(y, 1.0, opts);
        if (modes.empty()) return std::nan("");

        const double h = static_cast<double>(params_.horizon);
        const double jEnd = static_cast<double>(W - 1);
        std::complex<double> now(0.0, 0.0), future(0.0, 0.0);
        for (const auto& m : modes) {
            std::complex<double> z = std::exp(m.omega);
            double growth = std::pow(std::abs(z), jEnd + h);
            if (!std::isfinite(growth) || growth > params_.maxModeGrowth) continue;
            now += m.amplitude * std::pow(z, jEnd);
            future += m.amplitude * std::pow(z, jEnd + h);
        }
        double move = future.real() - now.real();
        if (params_.detrend) move += b * h;   // the trend continues too
        if (!std::isfinite(move) || std::fabs(move) > params_.maxPredictedMove)
            return std::nan("");
        return move;
    }

    Signal onBar(const CandleSeries&, size_t i, const PositionContext& pos) override {
        if (i + 1 < static_cast<size_t>(params_.window)) return Signal::Hold;

        // Refitting every bar is an SVD per bar; the shape a pencil sees does
        // not change materially between adjacent bars, so refit on a cadence
        // and hold the last prediction in between. refitEvery = 1 recovers the
        // per-bar behaviour exactly.
        if (lastFit_ == static_cast<size_t>(-1) ||
            i - lastFit_ >= static_cast<size_t>(std::max(1, params_.refitEvery))) {
            double m = predictedMove(i);
            cachedSignal_ = m;
            lastFit_ = i;
        }
        predicted_[i] = cachedSignal_;
        if (!std::isfinite(cachedSignal_)) return Signal::Hold;

        if (cachedSignal_ > params_.entryThreshold) return Signal::Buy;
        if (cachedSignal_ < -params_.entryThreshold)
            return pos.inPosition ? Signal::Sell : Signal::Hold;
        return Signal::Hold;
    }

    const std::vector<double>& predictions() const { return predicted_; }

private:
    Params params_;
    std::vector<double> logClose_;
    mutable std::vector<double> predicted_;
    size_t lastFit_ = static_cast<size_t>(-1);
    double cachedSignal_ = 0.0;
};

} // namespace trader
