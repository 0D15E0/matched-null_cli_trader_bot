#pragma once
#include "../strategy.h"
#include "zoo_common.h"
#include <cmath>
#include <string>
#include <vector>

namespace trader {

// ---------------------------------------------------------------------------
// Faber (2007), "A Quantitative Approach to Tactical Asset Allocation",
// Journal of Wealth Management. The whole rule: hold the asset while its
// price is above its own N-period moving average, hold cash otherwise.
//
// It is here as the honest baseline for every more elaborate trend model in
// this zoo. Faber's claim was never that the 10-month SMA is special - it was
// that a simple, slow, long/flat trend filter cuts drawdown a lot while
// leaving most of the return. If a fifteen-parameter descendant of it cannot
// beat this on out-of-sample excess Sharpe, the extra parameters are noise.
// ---------------------------------------------------------------------------
struct FaberParams {
    int window = 200;      // Faber's 10 months ~ 200 trading days
    double bandPct = 0.0;  // dead band, in percent of the MA, to damp whipsaws
};

class FaberMaStrategy : public Strategy {
public:
    explicit FaberMaStrategy(FaberParams p = {}) : p_(p) {}
    std::string name() const override { return "faber_ma"; }

    void prepare(const CandleSeries& s) override {
        ma_ = indicators::sma(s.close, std::max(2, p_.window));
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext&) override {
        if (i >= ma_.size() || !zoo::ok(ma_[i])) return Signal::Hold;
        double band = ma_[i] * p_.bandPct / 100.0;
        if (s.close[i] > ma_[i] + band) return Signal::Buy;
        if (s.close[i] < ma_[i] - band) return Signal::Sell;
        return Signal::Hold;
    }

private:
    FaberParams p_;
    std::vector<double> ma_;
};

// ---------------------------------------------------------------------------
// Kaufman's Adaptive Moving Average (Kaufman, "Smarter Trading", 1995).
//
// The smoothing constant is driven by the EFFICIENCY RATIO - net directional
// movement divided by total path length over the same window. A straight-line
// move has ER ~ 1 and the average tracks price almost immediately; a choppy
// move covering the same ground has ER ~ 0 and the average nearly freezes.
// So the filter is fast exactly when a trend rule wants to be fast and slow
// exactly when it would otherwise whipsaw, which is the failure mode every
// fixed-window moving average has.
//
// ER is a ratio of distances in the same units, so it is scale-free by
// construction and a threshold on it means the same thing on any instrument.
// ---------------------------------------------------------------------------
struct KamaParams {
    int erWindow = 20;      // window for the efficiency ratio
    int fastPeriod = 2;     // EMA period at ER = 1
    int slowPeriod = 30;    // EMA period at ER = 0
    double minEfficiency = 0.0; // refuse entries below this ER (0 = off)
};

class KamaTrendStrategy : public Strategy {
public:
    explicit KamaTrendStrategy(KamaParams p = {}) : p_(p) {}
    std::string name() const override { return "kama_trend"; }

    void prepare(const CandleSeries& s) override {
        const auto& c = s.close;
        size_t n = c.size();
        int w = std::max(2, p_.erWindow);
        kama_.assign(n, std::nan(""));
        er_.assign(n, std::nan(""));
        double fastSc = 2.0 / (std::max(1, p_.fastPeriod) + 1.0);
        double slowSc = 2.0 / (std::max(2, p_.slowPeriod) + 1.0);

        // Rolling sum of |delta| for the denominator, so the whole series is
        // O(n) rather than O(n*window).
        double pathSum = 0.0;
        for (size_t i = 1; i < n; ++i) {
            pathSum += std::fabs(c[i] - c[i - 1]);
            if (i > static_cast<size_t>(w)) pathSum -= std::fabs(c[i - w] - c[i - w - 1]);
            if (i < static_cast<size_t>(w)) continue;
            double direction = std::fabs(c[i] - c[i - w]);
            double er = pathSum > 0.0 ? direction / pathSum : 0.0;
            er_[i] = er;
            double sc = er * (fastSc - slowSc) + slowSc;
            sc *= sc;                       // Kaufman squares it
            double prev = zoo::ok(kama_[i - 1]) ? kama_[i - 1] : c[i - w];
            kama_[i] = prev + sc * (c[i] - prev);
        }
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext&) override {
        if (i == 0 || i >= kama_.size() || !zoo::ok(kama_[i]) || !zoo::ok(kama_[i - 1]))
            return Signal::Hold;
        bool rising = kama_[i] > kama_[i - 1];
        bool efficient = !zoo::ok(er_[i]) || er_[i] >= p_.minEfficiency;
        if (s.close[i] > kama_[i] && rising && efficient) return Signal::Buy;
        if (s.close[i] < kama_[i] || !rising) return Signal::Sell;
        return Signal::Hold;
    }

private:
    KamaParams p_;
    std::vector<double> kama_, er_;
};

// ---------------------------------------------------------------------------
// Ehlers' SuperSmoother (Ehlers, "Cybernetic Analysis for Stocks and
// Futures", 2004): a two-pole Butterworth low-pass filter applied to price.
//
// The reason to prefer it over an SMA is aliasing. An N-bar SMA has large
// side lobes in its frequency response, so cycles shorter than the window
// are not removed - they are folded back into the output as spurious slow
// wiggles, and a slope-of-the-average rule then trades them. A Butterworth
// response rolls off monotonically, so the slope of this filter is much
// closer to "the slope of the trend" and much less "an artifact of the
// smoother". Lag is comparable to an EMA of similar cutoff.
//
// The entry test is the filter's slope measured in units of the instrument's
// own volatility, not in price, so one threshold is meaningful everywhere.
// ---------------------------------------------------------------------------
struct EhlersParams {
    int cutoffPeriod = 20;   // cycles shorter than this are suppressed
    int slopeLag = 5;        // bars over which the slope is measured
    int volWindow = 60;      // window normalizing the slope
    double entrySigmas = 0.15;
};

class EhlersTrendStrategy : public Strategy {
public:
    explicit EhlersTrendStrategy(EhlersParams p = {}) : p_(p) {}
    std::string name() const override { return "ehlers_trend"; }

    void prepare(const CandleSeries& s) override {
        const auto& c = s.close;
        size_t n = c.size();
        filt_.assign(n, std::nan(""));
        slope_.assign(n, std::nan(""));
        double period = std::max(4, p_.cutoffPeriod);
        const double sqrt2 = 1.41421356237;
        double a1 = std::exp(-sqrt2 * M_PI / period);
        double b1 = 2.0 * a1 * std::cos(sqrt2 * M_PI / period);
        double c2 = b1, c3 = -a1 * a1, c1 = 1.0 - c2 - c3;
        for (size_t i = 0; i < n; ++i) {
            if (i < 2) { filt_[i] = c[i]; continue; }
            filt_[i] = c1 * (c[i] + c[i - 1]) / 2.0 + c2 * filt_[i - 1] + c3 * filt_[i - 2];
        }
        vol_ = indicators::rollingVolatility(c, std::max(5, p_.volWindow));
        int lag = std::max(1, p_.slopeLag);
        for (size_t i = static_cast<size_t>(lag); i < n; ++i) {
            if (filt_[i - lag] == 0.0 || !zoo::ok(vol_[i]) || vol_[i] <= 0.0) continue;
            // Log slope per bar, divided by per-bar volatility: a pure number.
            double logSlope = std::log(std::max(1e-12, filt_[i] / filt_[i - lag])) / lag;
            slope_[i] = logSlope / vol_[i];
        }
    }

    Signal onBar(const CandleSeries&, size_t i, const PositionContext&) override {
        if (i >= slope_.size() || !zoo::ok(slope_[i])) return Signal::Hold;
        if (slope_[i] > p_.entrySigmas) return Signal::Buy;
        if (slope_[i] < -p_.entrySigmas) return Signal::Sell;
        return Signal::Hold;
    }

private:
    EhlersParams p_;
    std::vector<double> filt_, slope_, vol_;
};

// ---------------------------------------------------------------------------
// The normalized MACD trend signal from Baz, Granger, Harvey, Le Roux &
// Rattray (2015), "Dissecting Investment Strategies in the Cross Section and
// Time Series" (SSRN 2695101) - the same construction Lim, Zohren & Roberts
// (arXiv:1904.04912) feed to their networks as the baseline momentum feature.
//
// Two normalizations, and both of them matter:
//   1. the raw EMA spread is divided by the rolling standard deviation of
//      PRICE, turning a currency amount into a number comparable across
//      instruments;
//   2. that quantity is divided by its OWN rolling standard deviation, so the
//      signal is a z-score of the trend measure rather than of the price.
// Without (2) a threshold means something different in a calm year than in a
// violent one on the same instrument.
// ---------------------------------------------------------------------------
struct MacdNormParams {
    int shortWindow = 24;
    int longWindow = 96;
    int priceStdWindow = 63;
    int signalStdWindow = 252;
    double entryZ = 0.20;
};

class MacdNormStrategy : public Strategy {
public:
    explicit MacdNormStrategy(MacdNormParams p = {}) : p_(p) {}
    std::string name() const override { return "macd_norm"; }

    void prepare(const CandleSeries& s) override {
        const auto& c = s.close;
        size_t n = c.size();
        int sw = std::max(2, p_.shortWindow);
        int lw = std::max(sw + 1, p_.longWindow);
        auto fast = indicators::ema(c, sw);
        auto slow = indicators::ema(c, lw);
        auto priceStd = indicators::rollingStd(c, std::max(5, p_.priceStdWindow));

        std::vector<double> q(n, std::nan(""));
        for (size_t i = 0; i < n; ++i)
            if (zoo::ok(fast[i]) && zoo::ok(slow[i]) && zoo::ok(priceStd[i]) && priceStd[i] > 0.0)
                q[i] = (fast[i] - slow[i]) / priceStd[i];

        auto qStd = indicators::rollingStd(q, std::max(10, p_.signalStdWindow));
        z_.assign(n, std::nan(""));
        for (size_t i = 0; i < n; ++i)
            if (zoo::ok(q[i]) && zoo::ok(qStd[i]) && qStd[i] > 0.0) z_[i] = q[i] / qStd[i];
    }

    Signal onBar(const CandleSeries&, size_t i, const PositionContext&) override {
        if (i >= z_.size() || !zoo::ok(z_[i])) return Signal::Hold;
        if (z_[i] > p_.entryZ) return Signal::Buy;
        if (z_[i] < -p_.entryZ) return Signal::Sell;
        return Signal::Hold;
    }

private:
    MacdNormParams p_;
    std::vector<double> z_;
};

} // namespace trader
