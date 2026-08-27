#pragma once
#include "../strategy.h"
#include "zoo_common.h"
#include <cmath>
#include <string>
#include <vector>

namespace trader {
namespace zoo {

// Rolling Lo-MacKinlay (1988) variance ratio, overlapping estimator.
//
//     VR(q) = Var[x_t - x_{t-q}] / (q * Var[x_t - x_{t-1}])
//
// Under a random walk VR(q) = 1 for every q. VR > 1 means q-bar moves are
// larger than q independent one-bar moves would be, i.e. positive
// autocorrelation - trends persist. VR < 1 means the opposite: moves partly
// undo themselves, and a reversal rule is the right instrument. This is one
// of the few statistics that says WHICH KIND of strategy a stretch of data
// can support, which is why it is worth more here than one more trend
// indicator.
//
// The overlapping form with the (1 - q/T) bias correction is used, as in the
// paper - the non-overlapping estimator throws away most of a short window
// and is far too noisy at the lengths a rolling filter can afford.
inline std::vector<double> rollingVarianceRatio(const std::vector<double>& close,
                                                 int window, int q) {
    auto x = logOf(close);
    size_t n = x.size();
    std::vector<double> out(n, std::nan(""));
    int T = std::max(4 * q, window);
    if (q < 2 || T < q + 4) return out;
    for (size_t i = static_cast<size_t>(T); i < n; ++i) {
        size_t s = i - static_cast<size_t>(T);
        if (!ok(x[s]) || !ok(x[i])) continue;
        double mu = (x[i] - x[s]) / T;
        double va = 0.0;
        bool clean = true;
        for (size_t t = s + 1; t <= i; ++t) {
            if (!ok(x[t]) || !ok(x[t - 1])) { clean = false; break; }
            double d = x[t] - x[t - 1] - mu;
            va += d * d;
        }
        if (!clean || va <= 0.0) continue;
        va /= (T - 1);
        double vc = 0.0;
        size_t count = 0;
        for (size_t t = s + static_cast<size_t>(q); t <= i; ++t) {
            double d = x[t] - x[t - static_cast<size_t>(q)] - q * mu;
            vc += d * d;
            ++count;
        }
        if (count == 0) continue;
        double m = static_cast<double>(q) * (T - q + 1) * (1.0 - static_cast<double>(q) / T);
        if (m <= 0.0) continue;
        vc /= m;
        out[i] = vc / (q * va);
    }
    return out;
}

// Rolling permutation entropy (Bandt & Pompe 2002, PRL 88:174102),
// normalized to [0,1].
//
// Every length-`order` window of the series is replaced by the PERMUTATION
// that sorts it - the ordinal pattern - and the Shannon entropy of the
// distribution of those patterns over a longer window is the statistic. It
// is invariant to any monotonic transformation of the data, needs no
// binning, is robust to outliers, and is cheap. Low entropy means the
// instrument's ordinal dynamics are repetitive and therefore forecastable;
// entropy at 1 means every ordering is equally likely, which is what an
// unpredictable series looks like.
//
// Used here as a gate: only take entries when the recent past has been
// structured enough to justify believing a pattern-based rule at all.
inline std::vector<double> rollingPermutationEntropy(const std::vector<double>& close,
                                                      int order, int window) {
    size_t n = close.size();
    std::vector<double> out(n, std::nan(""));
    int m = std::max(3, std::min(5, order));
    if (window < 10 * m) return out;
    // Factorial of m: 6, 24 or 120 patterns.
    int nPat = 1;
    for (int k = 2; k <= m; ++k) nPat *= k;
    const double logNPat = std::log(static_cast<double>(nPat));

    // Ordinal pattern index at each bar: a positional radix code of the
    // permutation that sorts close[i-m+1 .. i]. Only distinctness matters,
    // not which integer a given permutation gets, so the cheap radix
    // encoding is used rather than a Lehmer code.
    const size_t kCodeSpace = [&] { size_t v = 1; for (int k = 0; k < m; ++k) v *= static_cast<size_t>(m); return v; }();
    std::vector<int> pattern(n, -1);
    std::vector<int> idx(m);
    for (size_t i = static_cast<size_t>(m - 1); i < n; ++i) {
        bool clean = true;
        for (int k = 0; k < m; ++k) {
            if (!ok(close[i - m + 1 + k])) { clean = false; break; }
            idx[k] = k;
        }
        if (!clean) continue;
        std::sort(idx.begin(), idx.end(), [&](int a, int b) {
            double va = close[i - m + 1 + a], vb = close[i - m + 1 + b];
            return va != vb ? va < vb : a < b;
        });
        int code = 0;
        for (int k = 0; k < m; ++k) code = code * m + idx[k];
        pattern[i] = code;
    }

    // Sliding histogram: one pattern enters and one leaves per bar. The
    // obvious implementation - rebuild the counts over the whole window at
    // every bar - is O(n * window) with a large constant and was the second
    // most expensive thing in the search after the LPPLS fits.
    std::vector<int> counts(kCodeSpace, 0);
    size_t total = 0;
    for (size_t i = 0; i < n; ++i) {
        if (pattern[i] >= 0) { ++counts[static_cast<size_t>(pattern[i])]; ++total; }
        if (i >= static_cast<size_t>(window)) {
            size_t drop = i - static_cast<size_t>(window);
            if (pattern[drop] >= 0) { --counts[static_cast<size_t>(pattern[drop])]; --total; }
        }
        if (i + 1 < static_cast<size_t>(window)) continue;
        if (total * 2 < static_cast<size_t>(window)) continue;
        double h = 0.0;
        for (int c : counts) {
            if (c <= 0) continue;
            double p = static_cast<double>(c) / static_cast<double>(total);
            h -= p * std::log(p);
        }
        out[i] = h / logNPat;
    }
    return out;
}

// Intensity of a univariate Hawkes process with an exponential kernel, fitted
// by construction rather than by MLE (Hawkes 1971; Bacry, Mastromatteo &
// Muzy 2015, "Hawkes processes in finance", Market Microstructure and
// Liquidity 1(1)).
//
//     lambda_t = mu + sum_{t_i < t} alpha * exp(-beta (t - t_i))
//
// Events are bars whose return exceeds `jumpSigmas` local standard
// deviations. The recursion lambda_t = lambda_{t-1} * exp(-1/decay) +
// alpha * 1{event} makes this O(n) and strictly causal. What it measures is
// clustering: large moves in financial series arrive in bursts, and the
// self-excitation parameter is a direct reading of how much of the current
// hazard is inherited from recent shocks rather than from the baseline.
//
// A high reading is a warning, not a direction. Entering a trend position
// into a strongly self-excited regime is entering where the next large move
// is most likely and its sign is least predictable.
inline std::vector<double> hawkesIntensity(const std::vector<double>& close,
                                            int volWindow, double jumpSigmas, double decayBars) {
    auto rets = indicators::percentReturns(close);
    auto vol = indicators::rollingVolatility(close, std::max(10, volWindow));
    size_t n = close.size();
    std::vector<double> out(n, std::nan(""));
    double phi = std::exp(-1.0 / std::max(1.0, decayBars));
    double lambda = 0.0;
    for (size_t i = 0; i < n; ++i) {
        lambda *= phi;
        if (ok(rets[i]) && ok(vol[i]) && vol[i] > 0.0) {
            if (std::fabs(rets[i]) > jumpSigmas * vol[i]) lambda += 1.0;
            out[i] = lambda;
        }
    }
    return out;
}

} // namespace zoo

// ---------------------------------------------------------------------------
// Moreira & Muir (2017), "Volatility-Managed Portfolios", Journal of Finance
// 72(4): scaling exposure by the inverse of the previous period's realized
// variance raises the Sharpe ratio of essentially every standard factor.
// The mechanism is that volatility is strongly predictable at short horizons
// while expected return is not, so cutting exposure when variance is high
// removes risk that was not being paid for.
//
// This engine is long/flat, so continuous 1/sigma^2 scaling is not
// expressible as a Signal - that lives in the engine's --vol-target sizing
// instead. What IS expressible, and is what this strategy tests, is the
// discrete version of the same claim: stand aside entirely when realized
// variance is in the top of its own trailing distribution.
//
// The quantile is measured against the instrument's own history rather than
// set as an absolute level, so "high volatility" means high FOR THIS
// INSTRUMENT - the difference between a rule and a thinly disguised
// asset-class filter.
// ---------------------------------------------------------------------------
struct VolManagedParams {
    int volWindow = 20;
    int rankWindow = 250;
    double maxVolRank = 0.70;   // stay flat above this percentile of own vol
    int trendWindow = 100;      // 0 disables the trend gate
};

class VolManagedStrategy : public Strategy {
public:
    explicit VolManagedStrategy(VolManagedParams p = {}) : p_(p) {}
    std::string name() const override { return "vol_managed"; }

    void prepare(const CandleSeries& s) override {
        auto vol = indicators::rollingVolatility(s.close, std::max(5, p_.volWindow));
        rank_ = zoo::rollingPercentileRank(vol, std::max(30, p_.rankWindow));
        trend_ = p_.trendWindow > 1 ? indicators::sma(s.close, p_.trendWindow)
                                     : std::vector<double>(s.close.size(), std::nan(""));
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext&) override {
        if (i >= rank_.size() || !zoo::ok(rank_[i])) return Signal::Hold;
        bool calm = rank_[i] <= p_.maxVolRank;
        bool up = p_.trendWindow <= 1 || (zoo::ok(trend_[i]) && s.close[i] > trend_[i]);
        if (calm && up) return Signal::Buy;
        if (!calm || !up) return Signal::Sell;
        return Signal::Hold;
    }

private:
    VolManagedParams p_;
    std::vector<double> rank_, trend_;
};

// ---------------------------------------------------------------------------
// A regime-switching rule built directly on the Lo-MacKinlay variance ratio,
// in the spirit of Lo's Adaptive Markets Hypothesis (Lo 2004, JPM 30th
// Anniversary Issue): the profitability of a given rule is not a constant of
// the market but a function of which regime the market is currently in, and
// the sensible response is to measure the regime and switch.
//
// Concretely: measure VR(q) over a rolling window. Above 1 + band, run a
// trend rule. Below 1 - band, run a reversal rule. In the indeterminate
// middle, hold cash - because the honest answer there is that neither
// hypothesis is supported, and a strategy that always has an opinion is a
// strategy that trades noise most of the time.
// ---------------------------------------------------------------------------
struct VrSwitchParams {
    int vrWindow = 250;
    int vrLag = 5;
    double band = 0.15;
    int trendWindow = 50;
    int mrWindow = 20;
    double mrEntryZ = 1.5;
};

class VarianceRatioSwitchStrategy : public Strategy {
public:
    explicit VarianceRatioSwitchStrategy(VrSwitchParams p = {}) : p_(p) {}
    std::string name() const override { return "vr_switch"; }

    void prepare(const CandleSeries& s) override {
        vr_ = zoo::rollingVarianceRatio(s.close, std::max(40, p_.vrWindow), std::max(2, p_.vrLag));
        trend_ = indicators::sma(s.close, std::max(5, p_.trendWindow));
        int mw = std::max(5, p_.mrWindow);
        mrMean_ = indicators::sma(s.close, mw);
        mrStd_ = indicators::rollingStd(s.close, mw);
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext& pos) override {
        if (i >= vr_.size() || !zoo::ok(vr_[i])) return Signal::Hold;
        double v = vr_[i];
        if (v > 1.0 + p_.band) {                       // persistent regime
            if (!zoo::ok(trend_[i])) return Signal::Hold;
            return s.close[i] > trend_[i] ? Signal::Buy : Signal::Sell;
        }
        if (v < 1.0 - p_.band) {                       // mean-reverting regime
            if (!zoo::ok(mrMean_[i]) || !zoo::ok(mrStd_[i]) || mrStd_[i] <= 0.0)
                return Signal::Hold;
            double z = (s.close[i] - mrMean_[i]) / mrStd_[i];
            if (!pos.inPosition && z < -p_.mrEntryZ) return Signal::Buy;
            if (pos.inPosition && z > 0.0) return Signal::Sell;
            return Signal::Hold;
        }
        return Signal::Sell;                           // neither hypothesis supported
    }

private:
    VrSwitchParams p_;
    std::vector<double> vr_, trend_, mrMean_, mrStd_;
};

} // namespace trader
