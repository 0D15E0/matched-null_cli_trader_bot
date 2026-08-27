#pragma once
#include <cmath>
#include <cstddef>
#include <vector>

// Statistics shared by the backtest engine and the evolutionary searches.
//
// The single most consequential bug this module exists to prevent: the
// previous engine reported `(mean/stddev) * sqrt(totalBarCount)` and called
// it a Sharpe ratio. That expression is the t-statistic of the mean bar
// return - it grows without bound as you lengthen the backtest, so an
// 11-year 4h BTC run scored ~3.4x higher than an identical strategy
// measured over one year, and buy-and-hold BTC "scored" 3.56 by the same
// formula. Every number here is therefore annualized against the series'
// measured bars-per-year, and every Sharpe is reported alongside its
// standard error.
//
// Read that standard error correctly: it is driven by how much CALENDAR TIME
// the sample covers, not by how many trades were taken. It scales as
// sqrt((1 + SR^2/2) * barsPerYear / bars), and bars/barsPerYear is simply the
// span in years - so ten years of daily bars and ten years of 4h bars give a
// similar figure whether the strategy traded 8 times or 800. A low trade count
// is a *separate* weakness (the return distribution is then dominated by a
// handful of events, degrading the near-Gaussian assumption behind this
// formula), which is why BacktestReport::print() flags it on its own rather
// than pretending the error bar already accounts for it.
namespace trader::metrics {

constexpr double kEulerMascheroni = 0.5772156649015329;

inline double normalCdf(double x) {
    return 0.5 * std::erfc(-x * M_SQRT1_2);
}

// Acklam's rational approximation to the inverse normal CDF (|error| < 1.15e-9),
// used for the expected-maximum-of-N-trials correction below.
inline double normalInvCdf(double p) {
    if (p <= 0.0) return -INFINITY;
    if (p >= 1.0) return INFINITY;
    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02,
                               -2.759285104469687e+02, 1.383577518672690e+02,
                               -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02,
                               -1.556989798598866e+02, 6.680131188771972e+01,
                               -1.328068155288572e+01};
    static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01,
                               -2.400758277161838e+00, -2.549732539343734e+00,
                               4.374664141464968e+00,  2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01,
                               2.445134137142996e+00, 3.754408661907416e+00};
    const double pLow = 0.02425, pHigh = 1.0 - pLow;
    double q, r;
    if (p < pLow) {
        q = std::sqrt(-2.0 * std::log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    if (p > pHigh) {
        q = std::sqrt(-2.0 * std::log(1.0 - p));
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    q = p - 0.5;
    r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

// Moments of a per-bar return series. Skew/kurtosis are carried because the
// probabilistic Sharpe ratio needs them: trend-following returns are sharply
// non-normal (rare large winners, many small losers), and a Sharpe estimate
// from such a series is less reliable than the Gaussian formula suggests.
struct ReturnStats {
    size_t n = 0;
    double mean = 0.0;
    double stddev = 0.0;      // population stddev of per-bar returns
    double downsideDev = 0.0; // stddev of negative returns only (Sortino)
    double skew = 0.0;
    double kurtosis = 3.0;    // non-excess

    bool valid() const { return n > 1 && stddev > 0.0; }
    double sharpePerBar() const { return valid() ? mean / stddev : 0.0; }
};

inline ReturnStats computeReturnStats(const std::vector<double>& returns) {
    ReturnStats s;
    s.n = returns.size();
    if (s.n == 0) return s;
    for (double r : returns) s.mean += r;
    s.mean /= static_cast<double>(s.n);

    double m2 = 0.0, m3 = 0.0, m4 = 0.0, downside = 0.0;
    size_t downsideCount = 0;
    for (double r : returns) {
        double d = r - s.mean;
        double d2 = d * d;
        m2 += d2;
        m3 += d2 * d;
        m4 += d2 * d2;
        if (r < 0.0) { downside += r * r; ++downsideCount; }
    }
    double nD = static_cast<double>(s.n);
    m2 /= nD; m3 /= nD; m4 /= nD;
    s.stddev = std::sqrt(m2);
    if (m2 > 0.0) {
        s.skew = m3 / std::pow(m2, 1.5);
        s.kurtosis = m4 / (m2 * m2);
    }
    // Downside deviation is taken against zero (not the mean), the usual
    // Sortino convention: what matters is losing money, not underperforming
    // your own average.
    s.downsideDev = downsideCount > 0 ? std::sqrt(downside / nD) : 0.0;
    return s;
}

inline double annualizedSharpe(const ReturnStats& s, double barsPerYear) {
    if (!s.valid() || barsPerYear <= 0.0) return 0.0;
    return s.sharpePerBar() * std::sqrt(barsPerYear);
}

inline double annualizedSortino(const ReturnStats& s, double barsPerYear) {
    if (s.n < 2 || s.downsideDev <= 0.0 || barsPerYear <= 0.0) return 0.0;
    return (s.mean / s.downsideDev) * std::sqrt(barsPerYear);
}

// Standard error of the annualized Sharpe estimate (Lo, 2002). Reported
// next to every Sharpe so a headline number computed from a handful of
// trades is visibly indistinguishable from zero instead of looking like a
// finding.
inline double annualizedSharpeStdError(const ReturnStats& s, double barsPerYear) {
    if (!s.valid() || s.n < 3 || barsPerYear <= 0.0) return 0.0;
    double srp = s.sharpePerBar();
    double var = (1.0 + 0.5 * srp * srp) / (static_cast<double>(s.n) - 1.0);
    return std::sqrt(var * barsPerYear);
}

// Expected maximum of `numTrials` independent Sharpe estimates drawn from a
// population whose true Sharpe is zero (Bailey & Lopez de Prado). This is
// the bar a search result must clear to mean anything: run 2,400 random
// genomes against one price series and the best one will post a Sharpe
// around this value by luck alone.
inline double expectedMaxSharpeUnderNull(size_t numTrials, const ReturnStats& s,
                                          double barsPerYear) {
    if (numTrials < 2 || !s.valid()) return 0.0;
    double n = static_cast<double>(numTrials);
    double z1 = normalInvCdf(1.0 - 1.0 / n);
    double z2 = normalInvCdf(1.0 - 1.0 / (n * M_E));
    double expectedMaxZ = (1.0 - kEulerMascheroni) * z1 + kEulerMascheroni * z2;
    return expectedMaxZ * annualizedSharpeStdError(s, barsPerYear);
}

// Probabilistic Sharpe Ratio: the probability that the true Sharpe exceeds
// `benchmarkAnnualSharpe`, accounting for sample length, skew and kurtosis.
inline double probabilisticSharpe(const ReturnStats& s, double barsPerYear,
                                   double benchmarkAnnualSharpe) {
    if (!s.valid() || s.n < 3 || barsPerYear <= 0.0) return 0.0;
    double sr = s.sharpePerBar();
    double srStar = benchmarkAnnualSharpe / std::sqrt(barsPerYear);
    double denom = 1.0 - s.skew * sr + 0.25 * (s.kurtosis - 1.0) * sr * sr;
    if (denom <= 0.0) return 0.0;
    double z = (sr - srStar) * std::sqrt(static_cast<double>(s.n) - 1.0) / std::sqrt(denom);
    return normalCdf(z);
}

// Deflated Sharpe Ratio: the probability the strategy's true Sharpe is
// positive *after* accounting for the fact that it was selected as the best
// of `numTrials` attempts. A DSR below ~0.95 means the result is not
// distinguishable from the luckiest draw of a random search.
inline double deflatedSharpe(const ReturnStats& s, double barsPerYear, size_t numTrials) {
    if (!s.valid()) return 0.0;
    double threshold = expectedMaxSharpeUnderNull(numTrials, s, barsPerYear);
    return probabilisticSharpe(s, barsPerYear, threshold);
}

// Compound annual growth rate, in percent.
inline double cagrPct(double startEquity, double endEquity, double years) {
    if (startEquity <= 0.0 || endEquity <= 0.0 || years <= 0.0) return 0.0;
    return (std::pow(endEquity / startEquity, 1.0 / years) - 1.0) * 100.0;
}

} // namespace trader::metrics
