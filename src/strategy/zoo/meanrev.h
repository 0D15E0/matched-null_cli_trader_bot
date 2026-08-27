#pragma once
#include "../strategy.h"
#include "zoo_common.h"
#include <cmath>
#include <string>
#include <vector>

namespace trader {

// ---------------------------------------------------------------------------
// Short-horizon reversal, in the form popularized by Connors & Alvarez
// ("Short Term Trading Strategies That Work", 2008) and grounded in the
// academic short-term reversal literature (Jegadeesh 1990, JF; Lehmann 1990,
// QJE): over horizons of days, past losers outperform past winners - the
// opposite sign to the 12-month momentum effect.
//
// The long-term trend filter is not decoration. Buying dips is a bet that the
// dip is noise around a level, and in a genuine downtrend it is not; the
// SMA(trendWindow) gate is what stops the rule from averaging into a
// collapse. It is also what makes this a distinct hypothesis from `donchian`
// rather than its mirror image.
// ---------------------------------------------------------------------------
struct Rsi2Params {
    int rsiWindow = 2;
    double entryLevel = 10.0;   // buy when RSI dips below this
    double exitLevel = 70.0;    // sell when RSI recovers past this
    int trendWindow = 200;      // only buy above this moving average
    int maxHoldBars = 20;       // vertical barrier: reversal that never reverts
};

class Rsi2Strategy : public Strategy {
public:
    explicit Rsi2Strategy(Rsi2Params p = {}) : p_(p) {}
    std::string name() const override { return "rsi_reversal"; }

    void prepare(const CandleSeries& s) override {
        rsi_ = indicators::rsi(s.close, std::max(2, p_.rsiWindow));
        trend_ = indicators::sma(s.close, std::max(5, p_.trendWindow));
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext& pos) override {
        if (i >= rsi_.size() || !zoo::ok(rsi_[i])) return Signal::Hold;
        if (pos.inPosition) {
            if (rsi_[i] > p_.exitLevel) return Signal::Sell;
            if (p_.maxHoldBars > 0 && i >= pos.entryIndex + static_cast<size_t>(p_.maxHoldBars))
                return Signal::Sell;
            return Signal::Hold;
        }
        if (!zoo::ok(trend_[i])) return Signal::Hold;
        if (rsi_[i] < p_.entryLevel && s.close[i] > trend_[i]) return Signal::Buy;
        return Signal::Hold;
    }

private:
    Rsi2Params p_;
    std::vector<double> rsi_, trend_;
};

// ---------------------------------------------------------------------------
// The Ornstein-Uhlenbeck "s-score" of Avellaneda & Lee (2010), "Statistical
// Arbitrage in the U.S. Equities Market", Quantitative Finance 10(7).
//
// Their construction: strip an asset's factor exposure, cumulate what is
// left into a residual process X_t, fit an OU process to it, and trade the
// standardized distance of X from its own equilibrium:
//
//     dX = kappa (m - X) dt + sigma dW,     s = (X - m) / sigma_eq
//
// fitted through the AR(1) regression X_{j+1} = a + b X_j + zeta, from which
// m = a/(1-b), sigma_eq = sqrt(Var(zeta)/(1-b^2)), and the mean-reversion
// half-life is -ln2/ln(b). Open a long at s < -entryS, close it at
// s > -exitS. Their published cutoffs were 1.25 and 0.50 and are the
// defaults here.
//
// Single-instrument adaptation: with no cross-section to regress against,
// the residual is the log price minus its own slow moving average - the
// detrended log price. That keeps the object the paper's machinery actually
// needs (something plausibly stationary) without inventing a factor model.
//
// The half-life gate is the part most re-implementations drop and the part
// that does the work. If b is near 1 the series is not mean-reverting at all,
// and s is then a z-score of a random walk: it will drift to -2 and keep
// going. Avellaneda & Lee required reversion fast relative to the estimation
// window for exactly this reason, and so does this.
// ---------------------------------------------------------------------------
struct OuScoreParams {
    int window = 60;             // AR(1) estimation window, in bars
    int detrendWindow = 60;      // moving average defining the residual
    double entryS = 1.25;        // Avellaneda-Lee s_bo
    double exitS = 0.50;         // Avellaneda-Lee s_so
    double maxHalfLifeFrac = 0.5;// reject fits slower than this fraction of the window
};

class OuScoreStrategy : public Strategy {
public:
    explicit OuScoreStrategy(OuScoreParams p = {}) : p_(p) {}
    std::string name() const override { return "ou_score"; }

    void prepare(const CandleSeries& s) override {
        const auto logC = zoo::logOf(s.close);
        size_t n = logC.size();
        int dw = std::max(5, p_.detrendWindow);
        auto trend = indicators::sma(logC, dw);

        std::vector<double> resid(n, std::nan(""));
        for (size_t i = 0; i < n; ++i)
            if (zoo::ok(logC[i]) && zoo::ok(trend[i])) resid[i] = logC[i] - trend[i];

        int w = std::max(20, p_.window);
        score_.assign(n, std::nan(""));
        double maxHalfLife = p_.maxHalfLifeFrac * w;
        std::vector<double> x, y;
        x.reserve(w); y.reserve(w);
        for (size_t i = static_cast<size_t>(w); i < n; ++i) {
            x.clear(); y.clear();
            bool clean = true;
            for (size_t j = i - static_cast<size_t>(w); j < i; ++j) {
                if (!zoo::ok(resid[j]) || !zoo::ok(resid[j + 1])) { clean = false; break; }
                x.push_back(resid[j]);
                y.push_back(resid[j + 1]);
            }
            if (!clean) continue;
            double a, b, residVar;
            if (!zoo::ols(x, y, a, b, residVar)) continue;
            if (!(b > 0.0 && b < 1.0)) continue;            // not mean-reverting
            double halfLife = -std::log(2.0) / std::log(b);
            if (!(halfLife > 0.0 && halfLife <= maxHalfLife)) continue;
            double m = a / (1.0 - b);
            double sigmaEq = std::sqrt(residVar / (1.0 - b * b));
            if (!(sigmaEq > 0.0)) continue;
            score_[i] = (resid[i] - m) / sigmaEq;
        }
    }

    Signal onBar(const CandleSeries&, size_t i, const PositionContext& pos) override {
        if (i >= score_.size() || !zoo::ok(score_[i])) return Signal::Hold;
        if (pos.inPosition) return score_[i] > -p_.exitS ? Signal::Sell : Signal::Hold;
        return score_[i] < -p_.entryS ? Signal::Buy : Signal::Hold;
    }

private:
    OuScoreParams p_;
    std::vector<double> score_;
};

// ---------------------------------------------------------------------------
// Realized-skewness reversal, after Amaya, Christoffersen, Jacobs & Vasquez
// (2015), "Does Realized Skewness Predict the Cross-Section of Equity
// Returns?", Journal of Financial Economics 118(1).
//
// Their finding: weekly realized skewness predicts next-week returns with a
// NEGATIVE sign - stocks whose recent return distribution was most
// right-skewed do worst next. The usual reading is a lottery-preference
// story: investors overpay for the right tail.
//
// The time-series translation used here buys after the instrument's own
// realized skewness has gone sharply negative and holds for a fixed horizon.
// Skewness is dimensionless, so the threshold transfers across instruments
// without rescaling - one of the few signals in this zoo where that is free.
// ---------------------------------------------------------------------------
struct SkewReversalParams {
    int window = 20;            // bars of returns the skew is measured over
    double entrySkew = -0.5;    // buy when realized skew is below this
    int holdBars = 10;          // fixed holding horizon
    int trendWindow = 0;        // optional regime gate; 0 = off
};

class SkewReversalStrategy : public Strategy {
public:
    explicit SkewReversalStrategy(SkewReversalParams p = {}) : p_(p) {}
    std::string name() const override { return "skew_reversal"; }

    void prepare(const CandleSeries& s) override {
        auto rets = indicators::percentReturns(s.close);
        size_t n = rets.size();
        int w = std::max(6, p_.window);
        skew_.assign(n, std::nan(""));
        for (size_t i = static_cast<size_t>(w); i < n; ++i) {
            double mean = 0.0; int cnt = 0;
            for (size_t j = i + 1 - static_cast<size_t>(w); j <= i; ++j) {
                if (!zoo::ok(rets[j])) { cnt = -1; break; }
                mean += rets[j]; ++cnt;
            }
            if (cnt <= 0) continue;
            mean /= cnt;
            double m2 = 0.0, m3 = 0.0;
            for (size_t j = i + 1 - static_cast<size_t>(w); j <= i; ++j) {
                double d = rets[j] - mean;
                m2 += d * d; m3 += d * d * d;
            }
            m2 /= cnt; m3 /= cnt;
            if (m2 > 0.0) skew_[i] = m3 / std::pow(m2, 1.5);
        }
        trend_ = p_.trendWindow > 1 ? indicators::sma(s.close, p_.trendWindow)
                                     : std::vector<double>(n, std::nan(""));
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext& pos) override {
        if (i >= skew_.size() || !zoo::ok(skew_[i])) return Signal::Hold;
        if (pos.inPosition) {
            if (i >= pos.entryIndex + static_cast<size_t>(std::max(1, p_.holdBars)))
                return Signal::Sell;
            return Signal::Hold;
        }
        if (p_.trendWindow > 1 && (!zoo::ok(trend_[i]) || s.close[i] < trend_[i]))
            return Signal::Hold;
        return skew_[i] < p_.entrySkew ? Signal::Buy : Signal::Hold;
    }

private:
    SkewReversalParams p_;
    std::vector<double> skew_, trend_;
};

} // namespace trader
