#pragma once
#include "strategy.h"
#include "../indicators/indicators.h"
#include <cmath>
#include <algorithm>

namespace trader {

// Time Series Momentum (TSMOM), following Moskowitz/Ooi/Pedersen and the
// volatility-scaling framework popularized in "Enhancing Time Series
// Momentum Strategies Using Deep Neural Networks" (Lim, Zohren & Roberts,
// arXiv:1904.04912, JFDS 2019) - minus the neural network: this is the
// simple, well-established base signal their paper builds on top of.
//
// Signal: sign of the trailing return over `lookbackWindow` bars.
//   trailingReturn = close[i] / close[i - lookbackWindow] - 1
//   Buy  if trailingReturn > +threshold (persistent uptrend)
//   Sell if trailingReturn < -threshold (persistent downtrend, or exit a
//        long position that's stopped trending)
//   Hold otherwise
//
// Unlike a naive fixed-size momentum strategy, entries are additionally
// gated by realized volatility: `rollingVolatility` (see indicators.h) is
// used to skip entries when recent volatility is above `maxVolatility`
// (i.e. don't chase momentum during a violent, likely-to-reverse spike) -
// a simplified stand-in for full inverse-vol *position sizing*, which
// isn't directly expressible through the current `Signal{Hold,Buy,Sell}`
// interface (no position-size parameter) but is worth revisiting if the
// interface grows one.
//
// This is intentionally a much simpler, cheaper baseline than `odiseo` -
// good for comparing "does the Ichimoku+DMI machinery in odiseo actually
// beat plain trailing-return momentum?" the same way `pure_ichimoku`
// answers "does the DMI/ADX filter + ATR stop actually help?".
// Tuning knobs for TsmomStrategy, defined outside the class body so its
// default member initializers can be used as a default constructor
// argument without running into "incomplete type" default-argument rules.
struct TsmomParams {
    int lookbackWindow = 90;     // bars used for the trailing-return signal
    int volWindow = 30;          // bars used for the volatility gate
    double entryThreshold = 0.05;// minimum |trailing return| to act on

    // Volatility ceiling for entries, expressed ANNUALIZED. It used to be a
    // raw per-bar standard deviation of 0.08, which is ~127% annualized on
    // daily bars and ~370% on 4h bars - levels essentially never reached, so
    // the documented "don't chase momentum during a violent spike"
    // protection did not exist at defaults. Stating it annualized makes the
    // same number mean the same thing on every timeframe, converted at
    // prepare() time using the series' measured bars-per-year.
    double maxAnnualVolatility = 0.60;

    // Entry threshold in units of the trailing return's OWN volatility, rather
    // than as an absolute percentage. 0 (the default) keeps `entryThreshold`.
    //
    // The absolute version has exactly the bug this struct already fixed once
    // for maxAnnualVolatility: 5% means completely different things across
    // instruments. Over a 90-bar lookback a trailing return has standard
    // deviation ~ sigma_bar * sqrt(90), so on BTC 4h (about 80% annualized) 5%
    // is well inside noise, while on SHY (1-3y treasuries, about 1.5%
    // annualized) it is a three-sigma event. Measured on a 28-instrument
    // multi-asset universe: SHY took ZERO trades in 12.35 years, and TIP, HYG,
    // IEF and LQD took 3 to 8, while SLV and USO took 21 and 22. The threshold
    // was silencing precisely the low-volatility assets that diversify best.
    //
    // Setting this to k requires |trailing return| > k * sigma_bar * sqrt(L),
    // which is the same statement about statistical significance on every
    // instrument and timeframe.
    double entryThresholdSigmas = 0.0;
};

class TsmomStrategy : public Strategy {
public:
    using Params = TsmomParams;

    explicit TsmomStrategy(Params params = {}) : params_(params) {}

    std::string name() const override { return "tsmom"; }

    void prepare(const CandleSeries& series) override {
        const auto& close = series.close;
        size_t n = close.size();
        trailingReturn_.assign(n, std::nan(""));
        for (size_t i = 0; i < n; ++i) {
            if (i < static_cast<size_t>(params_.lookbackWindow)) continue;
            size_t j = i - params_.lookbackWindow;
            if (close[j] == 0.0) continue;
            trailingReturn_[i] = close[i] / close[j] - 1.0;
        }
        volatility_ = indicators::rollingVolatility(close, params_.volWindow);

        // Per-bar entry threshold, either the absolute setting or a multiple of
        // the trailing return's own standard deviation.
        threshold_.assign(n, params_.entryThreshold);
        if (params_.entryThresholdSigmas > 0.0) {
            const double rootL = std::sqrt(static_cast<double>(params_.lookbackWindow));
            for (size_t i = 0; i < n; ++i) {
                threshold_[i] = std::isnan(volatility_[i])
                    ? std::nan("")
                    : params_.entryThresholdSigmas * volatility_[i] * rootL;
            }
        }

        // Convert the annualized ceiling into this series' per-bar scale.
        double barsPerYear = series.barsPerYear();
        maxBarVolatility_ = barsPerYear > 0.0
            ? params_.maxAnnualVolatility / std::sqrt(barsPerYear)
            : params_.maxAnnualVolatility;
    }

    Signal onBar(const CandleSeries&, size_t i, const PositionContext&) override {
        if (std::isnan(trailingReturn_[i])) return Signal::Hold;
        bool volOk = std::isnan(volatility_[i]) || volatility_[i] <= maxBarVolatility_;
        double ret = trailingReturn_[i];

        const double thr = threshold_[i];
        if (std::isnan(thr)) return Signal::Hold;   // threshold not yet measurable
        if (ret > thr && volOk) return Signal::Buy;
        if (ret < -thr) return Signal::Sell; // always allow exiting/going flat
        return Signal::Hold;
    }

private:
    Params params_;
    std::vector<double> trailingReturn_;
    std::vector<double> volatility_;
    std::vector<double> threshold_;
    double maxBarVolatility_ = 1.0;
};

} // namespace trader
