#pragma once
#include "../strategy.h"
#include "zoo_common.h"
#include <cmath>
#include <string>
#include <vector>

namespace trader {

// ---------------------------------------------------------------------------
// Fractional differentiation, from Lopez de Prado, "Advances in Financial
// Machine Learning" (2018), chapter 5.
//
// The dilemma the chapter opens with: price series carry memory but are not
// stationary, and the usual fix - taking returns - is a full first
// difference that erases essentially all of that memory. Fractional
// differentiation of order d in (0,1) is the continuum in between. The
// binomial expansion of (1 - B)^d gives weights
//
//     w_0 = 1,   w_k = -w_{k-1} * (d - k + 1) / k
//
// which decay as a power law, so the filtered series keeps long-range
// structure while being far closer to stationary than the raw price.
// The fixed-width variant used here truncates the weight vector where
// |w_k| < tau, giving every output the same finite memory rather than a
// growing one (chapter 5.5) - which matters because an expanding-window
// version makes the early and late parts of a backtest incomparable.
//
// As a trading rule this is one signal with two regimes selected by d, and
// that is the interesting part: at d near 1 the output is essentially a
// return and its sign is momentum; at d near 0 it is essentially a deviation
// from a long-run level and its sign is mean reversion. `momentum` picks
// which reading is traded, so the search can discover which side of that
// continuum an instrument lives on rather than being told.
// ---------------------------------------------------------------------------
struct FracDiffParams {
    double d = 0.4;          // differentiation order
    double weightTau = 1e-4; // weight-magnitude cutoff for the fixed window
    int zWindow = 100;       // window standardizing the filtered series
    double entryZ = 0.5;
    double exitZ = 0.0;
    int momentum = 1;        // 1: trade the sign; 0: trade against it
};

class FracDiffStrategy : public Strategy {
public:
    explicit FracDiffStrategy(FracDiffParams p = {}) : p_(p) {}
    std::string name() const override { return "fracdiff"; }

    void prepare(const CandleSeries& s) override {
        // Binomial weights, truncated where they stop mattering. Capped at
        // 400 so an aggressive tau cannot turn this into an O(n^2) filter.
        std::vector<double> w{1.0};
        for (int k = 1; k < 400; ++k) {
            double next = -w.back() * (p_.d - k + 1.0) / k;
            if (std::fabs(next) < p_.weightTau) break;
            w.push_back(next);
        }
        const auto logC = zoo::logOf(s.close);
        size_t n = logC.size();
        size_t K = w.size();
        std::vector<double> fd(n, std::nan(""));
        for (size_t i = K - 1; i < n; ++i) {
            double acc = 0.0;
            bool clean = true;
            for (size_t k = 0; k < K; ++k) {
                double v = logC[i - k];
                if (!zoo::ok(v)) { clean = false; break; }
                acc += w[k] * v;
            }
            if (clean) fd[i] = acc;
        }
        int zw = std::max(20, p_.zWindow);
        auto mean = zoo::rollingMeanSkipNan(fd, zw);
        auto sd = indicators::rollingStd(fd, zw);
        z_.assign(n, std::nan(""));
        for (size_t i = 0; i < n; ++i)
            if (zoo::ok(fd[i]) && zoo::ok(mean[i]) && zoo::ok(sd[i]) && sd[i] > 0.0)
                z_[i] = (fd[i] - mean[i]) / sd[i];
    }

    Signal onBar(const CandleSeries&, size_t i, const PositionContext& pos) override {
        if (i >= z_.size() || !zoo::ok(z_[i])) return Signal::Hold;
        double v = p_.momentum ? z_[i] : -z_[i];
        if (pos.inPosition) return v < p_.exitZ ? Signal::Sell : Signal::Hold;
        return v > p_.entryZ ? Signal::Buy : Signal::Hold;
    }

private:
    FracDiffParams p_;
    std::vector<double> z_;
};

// ---------------------------------------------------------------------------
// CUSUM event filter plus the triple-barrier exit, from the same book
// (chapters 2.5.2.1 and 3.2).
//
// The premise is that fixed-interval sampling is the wrong clock. Most bars
// carry no information, so a rule evaluated on every one of them spends its
// statistical power on noise. The CUSUM filter instead accumulates returns
// and fires only when the running sum departs from zero by more than a
// threshold - a filter that stays silent through drift and speaks on genuine
// runs. The threshold is set in units of local volatility, so it is one rule
// on every instrument and adapts as the instrument's regime changes.
//
// The exit is the triple barrier: a profit target and a stop, both placed at
// multiples of the volatility measured AT ENTRY, plus a vertical barrier that
// closes the trade after a fixed number of bars whatever happened. The
// vertical barrier is not a detail - without it, positions taken on an event
// that did not resolve are held indefinitely and the strategy quietly becomes
// buy-and-hold with extra steps.
//
// Only upside CUSUM breaches open a position: the engine is long/flat, so a
// downside event has nothing to express.
// ---------------------------------------------------------------------------
struct CusumParams {
    int volWindow = 50;       // volatility estimate feeding threshold and barriers
    double hSigmas = 2.0;     // CUSUM threshold, in units of per-bar volatility
    double ptMult = 2.0;      // profit-taking barrier, in entry-bar sigmas
    double slMult = 1.5;      // stop-loss barrier, in entry-bar sigmas
    int maxHold = 30;         // vertical barrier, in bars
};

class CusumTripleBarrierStrategy : public Strategy {
public:
    explicit CusumTripleBarrierStrategy(CusumParams p = {}) : p_(p) {}
    std::string name() const override { return "cusum_tb"; }

    void prepare(const CandleSeries& s) override {
        auto rets = indicators::percentReturns(s.close);
        vol_ = indicators::rollingVolatility(s.close, std::max(10, p_.volWindow));
        size_t n = s.close.size();
        event_.assign(n, false);

        // The CUSUM accumulator is a pure function of price history, so it is
        // unrolled here once. Recomputing it inside onBar would either be
        // quadratic or require mutable state, and mutable state inside onBar
        // is what made the pre-2026 strategies unable to replay.
        double sPos = 0.0, sNeg = 0.0;
        for (size_t i = 1; i < n; ++i) {
            if (!zoo::ok(rets[i]) || !zoo::ok(vol_[i]) || vol_[i] <= 0.0) continue;
            double h = p_.hSigmas * vol_[i];
            sPos = std::max(0.0, sPos + rets[i]);
            sNeg = std::min(0.0, sNeg + rets[i]);
            if (sPos > h) { sPos = 0.0; event_[i] = true; }
            else if (sNeg < -h) { sNeg = 0.0; }
        }
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext& pos) override {
        if (i >= event_.size()) return Signal::Hold;
        if (pos.inPosition) {
            if (i >= pos.entryIndex + static_cast<size_t>(std::max(1, p_.maxHold)))
                return Signal::Sell;                       // vertical barrier
            double sigma = pos.entryIndex < vol_.size() ? vol_[pos.entryIndex] : std::nan("");
            if (!zoo::ok(sigma) || sigma <= 0.0 || pos.entryPrice <= 0.0) return Signal::Hold;
            double ret = s.close[i] / pos.entryPrice - 1.0;
            if (ret >= p_.ptMult * sigma) return Signal::Sell;   // profit target
            if (ret <= -p_.slMult * sigma) return Signal::Sell;  // stop loss
            return Signal::Hold;
        }
        return event_[i] ? Signal::Buy : Signal::Hold;
    }

private:
    CusumParams p_;
    std::vector<double> vol_;
    std::vector<char> event_;
};

} // namespace trader
