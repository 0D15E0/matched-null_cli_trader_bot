#pragma once
#include "../../indicators/indicators.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Shared, strictly-causal helpers for the strategy zoo.
//
// Every function here obeys the same contract the rest of the repo's
// indicators do: output[i] may depend only on inputs at indices <= i, and is
// NaN until enough history exists. That contract is the only thing standing
// between a backtest and a look-ahead artifact, so it is restated on each
// function rather than assumed.
namespace trader::zoo {

inline bool ok(double v) { return !std::isnan(v) && std::isfinite(v); }

// Natural log of every element; non-positive inputs become NaN rather than
// -inf, so a bad tick cannot poison a downstream regression with an infinity.
inline std::vector<double> logOf(const std::vector<double>& v) {
    std::vector<double> out(v.size(), std::nan(""));
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] > 0.0) out[i] = std::log(v[i]);
    return out;
}

// Fraction of the trailing `window` observations (ending at and including i)
// that are <= values[i]. In [0,1]; NaN until the window is full.
//
// This is how a threshold on a quantity with no natural scale - bandwidth,
// intensity, entropy - is made to mean the same thing on BTC 4h and on SHY
// daily. A raw cutoff on such a quantity silently encodes the instrument.
inline std::vector<double> rollingPercentileRank(const std::vector<double>& values, int window) {
    size_t n = values.size();
    std::vector<double> out(n, std::nan(""));
    if (window < 2) return out;
    std::vector<double> buf;
    buf.reserve(static_cast<size_t>(window));
    for (size_t i = 0; i < n; ++i) {
        if (i + 1 < static_cast<size_t>(window) || !ok(values[i])) continue;
        buf.clear();
        size_t start = i + 1 - static_cast<size_t>(window);
        size_t valid = 0, below = 0;
        for (size_t j = start; j <= i; ++j) {
            if (!ok(values[j])) continue;
            ++valid;
            if (values[j] <= values[i]) ++below;
        }
        // Demand most of the window be real data: a rank computed from three
        // surviving points is a number, not a percentile.
        if (valid * 2 < static_cast<size_t>(window)) continue;
        out[i] = static_cast<double>(below) / static_cast<double>(valid);
    }
    return out;
}

// Rolling mean over `window` bars ending at i, skipping NaNs.
inline std::vector<double> rollingMeanSkipNan(const std::vector<double>& v, int window) {
    size_t n = v.size();
    std::vector<double> out(n, std::nan(""));
    if (window < 1) return out;
    for (size_t i = 0; i < n; ++i) {
        if (i + 1 < static_cast<size_t>(window)) continue;
        double sum = 0.0; size_t cnt = 0;
        for (size_t j = i + 1 - static_cast<size_t>(window); j <= i; ++j)
            if (ok(v[j])) { sum += v[j]; ++cnt; }
        if (cnt * 2 >= static_cast<size_t>(window)) out[i] = sum / static_cast<double>(cnt);
    }
    return out;
}

// Highest of `v` over the `window` bars ENDING AT i-1 (i.e. excluding the
// current bar). Donchian breakouts compare today's close against the prior
// channel; including today's own high makes the channel un-breakable by
// construction, which is a classic silent way to build a strategy that
// never trades.
inline std::vector<double> rollingMaxExclusive(const std::vector<double>& v, int window) {
    size_t n = v.size();
    std::vector<double> out(n, std::nan(""));
    if (window < 1) return out;
    for (size_t i = 0; i < n; ++i) {
        if (i < static_cast<size_t>(window)) continue;
        double m = -1e300;
        for (size_t j = i - static_cast<size_t>(window); j < i; ++j)
            if (ok(v[j])) m = std::max(m, v[j]);
        if (m > -1e299) out[i] = m;
    }
    return out;
}

inline std::vector<double> rollingMinExclusive(const std::vector<double>& v, int window) {
    size_t n = v.size();
    std::vector<double> out(n, std::nan(""));
    if (window < 1) return out;
    for (size_t i = 0; i < n; ++i) {
        if (i < static_cast<size_t>(window)) continue;
        double m = 1e300;
        for (size_t j = i - static_cast<size_t>(window); j < i; ++j)
            if (ok(v[j])) m = std::min(m, v[j]);
        if (m < 1e299) out[i] = m;
    }
    return out;
}

// Ordinary least squares y = a + b x over paired samples. Returns false if
// the design is degenerate (fewer than 3 points, or zero variance in x).
inline bool ols(const std::vector<double>& x, const std::vector<double>& y,
                double& a, double& b, double& residVar) {
    size_t n = std::min(x.size(), y.size());
    if (n < 3) return false;
    double sx = 0, sy = 0;
    for (size_t i = 0; i < n; ++i) { sx += x[i]; sy += y[i]; }
    double mx = sx / n, my = sy / n;
    double sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) { double dx = x[i] - mx; sxx += dx * dx; sxy += dx * (y[i] - my); }
    if (sxx <= 0.0) return false;
    b = sxy / sxx;
    a = my - b * mx;
    double ss = 0;
    for (size_t i = 0; i < n; ++i) { double r = y[i] - (a + b * x[i]); ss += r * r; }
    residVar = n > 2 ? ss / static_cast<double>(n - 2) : 0.0;
    return true;
}

// Deterministic 64-bit mix (SplitMix64 finalizer). Used by the random-entry
// CONTROL strategy so its "coin flips" are a pure function of (seed, bar
// timestamp) - the Strategy contract requires re-evaluating the same bar to
// return the same signal, which a stateful RNG would violate, and a control
// whose results move between runs cannot be a control.
inline uint64_t mix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
inline double uniform01(uint64_t seed, int64_t stamp) {
    return static_cast<double>(mix64(seed ^ static_cast<uint64_t>(stamp)) >> 11) /
           static_cast<double>(1ull << 53);
}

} // namespace trader::zoo
