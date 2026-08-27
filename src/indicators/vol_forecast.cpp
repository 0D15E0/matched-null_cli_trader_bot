#include "vol_forecast.h"
#include "indicators.h"
#include "../math/pencil.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace trader::indicators {

namespace {

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

// ---------------------------------------------------------------------------
// Why the regression is run on LOG variance.
//
// Realized variance is positive, right-skewed over roughly two orders of
// magnitude, and heteroskedastic in a way that scales with its own level. OLS
// on the raw variance is therefore fitted almost entirely by the handful of
// crisis bars (a 10% bar contributes 10,000x the leverage of a 0.1% bar), and
// nothing stops it predicting a NEGATIVE variance, which for a sizer that
// divides by volatility is not a rounding error but a catastrophic position.
// Logs fix all three: the errors become roughly homoskedastic, the fit is
// driven by the whole sample instead of its tail, and exp() cannot produce a
// negative forecast. This is standard practice (HAR-log; Corsi 2009 himself
// reports the log specification).
//
// The cost is that E[exp(y)] != exp(E[y]) - the Jensen gap. It is NOT
// negligible here and it is NOT skipped. The residual of log(r^2) around
// log(sigma^2) is dominated by chi-square-with-1-degree-of-freedom noise,
// whose log has mean -1.27 and variance pi^2/2 = 4.93, so a naive exp()
// back-transform under-predicts the conditional variance by a large constant
// factor. Measured, by disabling the correction and comparing mean forecast
// variance against mean realized variance on the GARCH calibration: the ratio
// falls from 0.98 to 0.30, i.e. the forecast is 3.3x too small in variance and
// 1.8x too small in volatility. Under vol targeting that is not a forecasting
// error at all - the de-meaned MSE is unchanged at 0.037 - it is a silent 1.8x
// increase in leverage, and an A/B against Trailing would be measuring the
// leverage rather than the forecast. The correction applied below is Duan's
// (1983) smearing estimator - multiply exp(yhat) by the fit-window mean of
// exp(residual) - which is consistent for E[variance] without assuming the
// residuals are Gaussian (they are emphatically not; they are log-chi-square).
// ---------------------------------------------------------------------------

// log(variance + eps) with eps = kEpsFrac * (mean variance over the fit
// window). A single flat or near-flat bar has r^2 ~ 0 and log(r^2) -> -inf,
// which one observation is enough to wreck a least-squares fit; with 4h crypto
// bars those exist. Adding a fixed fraction of the window's mean variance
// floors the left tail at log(mean) - 6.9 while shifting a typical (median)
// observation by 0.2%, and keeps the transform scale-free so the same constant
// works for a 4h BTC series and a daily equity series. It also guarantees the
// returned variance is strictly positive, so the sqrt() below is always real.
// The eps is deliberately NOT subtracted back out after exp(): at 0.1% of the
// unconditional variance the induced upward bias is ~0.05% in volatility,
// orders of magnitude below estimation noise, and subtracting it would
// introduce a degenerate case where the forecast variance can reach zero and
// the sizer divides by it.
constexpr double kEpsFrac = 1e-3;

// A smearing factor far outside this range means the residuals are not what
// this model thinks they are (a near-singular fit, or a window containing a
// price jump of several hundred percent). Refuse the fit rather than apply it.
constexpr double kMaxSmear = 100.0;

// The forecast is allowed to leave the range of in-sample fitted values by
// this much in log-variance, i.e. by a factor e^2 = 7.4 in variance / 2.7x in
// volatility. Beyond that an OLS extrapolation is not forecasting, it is
// running the linear fit off the end of the data it saw - and the dangerous
// direction is DOWNWARD, since an absurdly small volatility forecast asks the
// engine for the largest position it is allowed to take.
constexpr double kClampSlack = 2.0;

// The ACF needs to be estimated from meaningfully more data than the number of
// lags it reports, or the pencil is fitting sampling noise.
constexpr int kAcfBarsPerLag = 4;
constexpr int kMinAcfBars = 32;

// FractionalHAR memory parameter bounds.
//
// Below kDMin the series has no usable long memory and a power-law weighted
// average is the wrong shape: as d -> 0 the weights go to psi_j ~ 1/j, a
// harmonic average that is still enormously long-memoried, so a short-memory
// series estimated at d ~ 0 would be handed the LONGEST regressor in the file.
// The correct response to "no long memory here" is HAR, not d = 0, so an
// estimate below the floor is a fallback rather than a clamp.
//
// Above 0.5 the process is non-stationary and d is not identified, but the
// weight SHAPE is still the right one and only its normalization is at the
// limit, so the top of the range is a clamp rather than a fallback.
constexpr double kDMin = 0.05;
constexpr double kDMax = 0.49;
constexpr int kMinLagsForD = 8;

// ---------------------------------------------------------------------------
// Volatility proxy: squared log returns, i.e. realized variance per bar.
//
// Bars whose return cannot be formed (non-positive or non-finite prices, and
// bar 0, which has no predecessor) are recorded as "bad" rather than as NaN.
// A NaN would poison the prefix sums that make the horizon averages O(1), so
// instead the value is set to 0 and a prefix COUNT of bad bars lets any window
// be tested for contamination in O(1) as well.
// ---------------------------------------------------------------------------
struct Proxy {
    std::vector<double> rv;      // rv[t] = log-return(t)^2, 0 where invalid
    std::vector<double> prefix;  // prefix[t+1] = sum of rv[0..t]
    std::vector<int> badPrefix;  // badPrefix[t+1] = # invalid bars in [0..t]
};

Proxy buildProxy(const std::vector<double>& close) {
    const size_t n = close.size();
    Proxy p;
    p.rv.assign(n, 0.0);
    p.prefix.assign(n + 1, 0.0);
    p.badPrefix.assign(n + 1, 0);
    for (size_t t = 0; t < n; ++t) {
        bool bad = true;
        if (t >= 1) {
            double a = close[t - 1], b = close[t];
            if (a > 0.0 && b > 0.0 && std::isfinite(a) && std::isfinite(b)) {
                double r = std::log(b / a);
                if (std::isfinite(r)) {
                    p.rv[t] = r * r;
                    bad = false;
                }
            }
        }
        p.prefix[t + 1] = p.prefix[t] + p.rv[t];
        p.badPrefix[t + 1] = p.badPrefix[t] + (bad ? 1 : 0);
    }
    return p;
}

// True when every bar in [from, to] (inclusive) has a usable return.
inline bool spanOk(const Proxy& p, size_t from, size_t to) {
    return p.badPrefix[to + 1] == p.badPrefix[from];
}

// Mean realized variance over the h bars ending at t. Caller guarantees t >= h
// (so the window starts at bar 1 or later, never at the undefined bar 0).
inline double avgRv(const Proxy& p, size_t t, int h) {
    return (p.prefix[t + 1] - p.prefix[t + 1 - static_cast<size_t>(h)]) / h;
}

// Mean realized variance over the H bars FORWARD of t, i.e. bars t+1..t+H:
// the regression target. Caller guarantees t + H is in range.
//
// H == 1 returns p.rv[t+1] directly rather than differencing the prefix sums.
// Those are equal in exact arithmetic and NOT equal in floating point - a
// difference of two accumulated sums carries the rounding of everything before
// it - and H=1 is contractually required to reproduce the pre-horizon results
// bit for bit, because that is what keeps the earlier A/B valid.
inline double avgRvForward(const Proxy& p, size_t t, int H) {
    if (H == 1) return p.rv[t + 1];
    return (p.prefix[t + 1 + static_cast<size_t>(H)] - p.prefix[t + 1]) / H;
}

// First index s such that [s, to] contains no bad bars. badPrefix is
// non-decreasing, so this is a binary search rather than a backward scan.
size_t goodTailStart(const Proxy& p, size_t to) {
    int target = p.badPrefix[to + 1];
    auto last = p.badPrefix.begin() + static_cast<std::ptrdiff_t>(to) + 1;
    auto it = std::lower_bound(p.badPrefix.begin(), last, target);
    return static_cast<size_t>(it - p.badPrefix.begin());
}

// ---------------------------------------------------------------------------
// Small dense solve (n <= 3 after centering removes the intercept column).
// Gaussian elimination with partial pivoting; returns false on a pivot that is
// negligible relative to the matrix scale, which is how a degenerate design -
// two identical horizons, a perfectly flat window - is caught instead of
// producing coefficients of size 1e17.
// ---------------------------------------------------------------------------
bool solveSmall(double a[3][3], double b[3], int n, double out[3]) {
    double scale = 0.0;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) scale = std::max(scale, std::fabs(a[i][j]));
    if (!(scale > 0.0) || !std::isfinite(scale)) return false;

    for (int c = 0; c < n; ++c) {
        int piv = c;
        for (int r = c + 1; r < n; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-12 * scale) return false;
        if (piv != c) {
            for (int j = 0; j < n; ++j) std::swap(a[c][j], a[piv][j]);
            std::swap(b[c], b[piv]);
        }
        for (int r = c + 1; r < n; ++r) {
            double f = a[r][c] / a[c][c];
            if (f == 0.0) continue;
            for (int j = c; j < n; ++j) a[r][j] -= f * a[c][j];
            b[r] -= f * b[c];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double s = b[r];
        for (int j = r + 1; j < n; ++j) s -= a[r][j] * out[j];
        out[r] = s / a[r][r];
        if (!std::isfinite(out[r])) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// A fitted model. Everything in here was computed from bars at or before the
// refit bar - that is the whole causality guarantee, concentrated in one
// struct: predict() below reads nothing else.
// ---------------------------------------------------------------------------
struct Fit {
    bool valid = false;
    int nh = 0;                        // number of regressors actually used
    std::array<int, 3> h{{0, 0, 0}};   // span of each regressor, ascending
    // Regressor j averages realized variance over the h[j] bars ending at t.
    // Flat weights (HAR's buckets) unless powerLaw[j], in which case the
    // weights are `weights` - psi_k ~ (k+1)^(d-1), normalized to sum to 1 so it
    // stays an average and stays on the same scale as the flat buckets.
    std::array<bool, 3> powerLaw{{false, false, false}};
    std::vector<double> weights;
    int maxH = 0;
    int horizon = 1;                   // H: target is the mean variance over t+1..t+H
    double dHat = 0.0;                 // memory parameter used (0 if not fractional)
    double eps = 0.0;                  // log-offset, from the fit window's mean variance
    std::array<double, 3> meanX{{0.0, 0.0, 0.0}};
    double meanY = 0.0;
    std::array<double, 3> beta{{0.0, 0.0, 0.0}};
    double smear = 1.0;                // Duan back-transform correction
    double lo = 0.0, hi = 0.0;         // clamp range for the log-variance forecast
};

// Value of regressor j at bar t. Flat buckets are an O(1) prefix-sum lookup;
// the power-law term is an O(span) dot product, which is why FractionalHAR
// costs more than HAR but still nothing like the pencil's per-refit SVD.
inline double regressorValue(const Proxy& p, size_t t, const Fit& f, int j) {
    const int span = f.h[static_cast<size_t>(j)];
    if (!f.powerLaw[static_cast<size_t>(j)]) return avgRv(p, t, span);
    double acc = 0.0;
    for (int k = 0; k < span; ++k) acc += f.weights[static_cast<size_t>(k)] * p.rv[t - static_cast<size_t>(k)];
    return acc;
}

// psi_k ~ (k+1)^(d-1), k = 0 .. span-1, normalized to sum 1. This is the
// weight profile of the infinite AR representation of a fractionally
// integrated process: the reason it matters is the TAIL, not the head. At the
// d ~ 0.38 measured on BTC the current bar carries only ~5% of the weight and
// the remainder is spread with no characteristic scale over the whole span.
//
// MEASURED: the shape is worth having, the term it replaces was worth more. At
// EQUAL span the power law beats a flat average convincingly (BTC H=141 QLIKE
// 0.414 vs 0.484 over 187 bars; AAPL -3.9% vs -0.2%), so fractional weighting
// is doing real work. But on BTC a long third regressor is the wrong thing to
// have at all - flat long buckets of 22/50/100/187 bars score 0.346/0.368/
// 0.407/0.484 - so beating the flat 187-bar version still loses to Corsi's 22.
// FractionalHAR wins only where the long term genuinely helps, which of the
// two shipped instruments is AAPL at H=141 and nowhere else.
void buildPowerLawWeights(double d, int span, std::vector<double>& w) {
    w.assign(static_cast<size_t>(span), 0.0);
    double sum = 0.0;
    for (int k = 0; k < span; ++k) {
        double v = std::pow(static_cast<double>(k) + 1.0, d - 1.0);
        w[static_cast<size_t>(k)] = v;
        sum += v;
    }
    if (!(sum > 0.0) || !std::isfinite(sum)) { w.clear(); return; }
    for (double& v : w) v /= sum;
}

// Ascending, distinct, clamped to [1, cap]. Duplicate horizons would make the
// design matrix exactly singular, so they are merged rather than rejected -
// PencilHAR can legitimately recover two modes that round to the same bar
// count, and the right response is a two-horizon HAR, not no forecast.
std::vector<int> sanitizeHorizons(const std::array<int, 3>& in, int cap) {
    std::vector<int> h;
    for (int v : in) {
        if (v <= 0) continue;
        h.push_back(std::min(v, cap));
    }
    std::sort(h.begin(), h.end());
    h.erase(std::unique(h.begin(), h.end()), h.end());
    if (h.size() > 3) h.resize(3);
    return h;
}

// The longest horizon the sizer is allowed to use. A horizon consumes warm-up
// bars and eats into the number of usable fit samples, so an unbounded
// 1/lambda from a nearly-non-decaying pencil mode (lambda -> 0 gives T -> inf)
// has to be capped somewhere; a quarter of the fit window keeps at least 3/4
// of it available as regression samples.
int horizonCap(const VolForecastConfig& cfg) {
    int byFit = std::max(2, cfg.fitWindow / 4);
    int byAcf = std::max(2, kAcfBarsPerLag * std::max(1, cfg.pencilAcfLags));
    return std::min(byFit, byAcf);
}

// ---------------------------------------------------------------------------
// PencilHAR timescale discovery, over bars [.., endBar] only.
//
// The autocorrelation is measured on |log return|, NOT on the realized
// variance the regression itself uses, and not on its log. All three carry the
// same decay rates - a one-bar proxy is (latent volatility) x (iid sampling
// noise), and the noise only attenuates every rho(k), k >= 1, by one common
// factor - so the choice is purely about signal-to-noise in a 750-bar window,
// and there it is not close. The attenuation is signal/(signal+noise) and the
// noise term is var(|z|) = 0.36 for |r| against var(log chi2_1) = 4.93 for the
// log proxy. Measured on the GARCH(1,1) calibration (out-of-sample MSE of the
// resulting log-variance forecast, lower better): |r| 0.069, r^2 0.088,
// log(rv) 0.122 - the log proxy, which is the obvious choice given the
// regression runs in logs, is the worst of the three by 77%. This is the same
// reason the long-memory literature measures decay on |r| rather than r^2
// (Ding, Granger & Engle 1993 - the "Taylor effect": the autocorrelation of
// |r|^d peaks near d = 1, not d = 2).
//
// Lag 0 is EXCLUDED from what is handed to the pencil. rho(0) = 1 is a
// property of the definition, not an estimate, and it sits far above the
// noise-attenuated tail; handing it over makes the estimator spend a mode on
// the gap between them (it comes back as T = 1). That is tempting - it looks
// like it reconstructs HAR's "daily" term for free - but it is measurably
// worse: including lag 0 costs 23% on the GARCH calibration (0.085 vs 0.069)
// and it destabilizes the recovered horizons on real data (AAPL daily drifts
// to 13-16 bars with lag 0, and sits at a steady 22-30 without it). What the
// pencil is being asked for is how volatility MEMORY decays, and the current
// bar is not memory.
// ---------------------------------------------------------------------------
// Autocorrelation of |log return| over the causal window ending at endBar.
// acf[k-1] = rho(k) for k = 1..lags. False when the window is unusable.
//
// Shared by both things that need to know the shape of volatility memory:
// PencilHAR's timescale discovery and FractionalHAR's memory-parameter
// estimate. They read the same numbers, so they can never disagree about what
// the series looks like, and the |r| choice is argued once (above) for both.
bool absReturnAcf(const Proxy& p, size_t endBar, int windowBars, int maxLag,
                  std::vector<double>& acf) {
    acf.clear();
    if (endBar < 1) return false;

    size_t start = goodTailStart(p, endBar);
    if (windowBars > 0 && endBar + 1 > static_cast<size_t>(windowBars))
        start = std::max(start, endBar + 1 - static_cast<size_t>(windowBars));
    if (start < 1) start = 1;  // bar 0 has no return
    if (endBar < start) return false;

    const size_t w = endBar - start + 1;
    if (w < static_cast<size_t>(kMinAcfBars)) return false;
    int lags = std::min(maxLag, static_cast<int>(w) / kAcfBarsPerLag);
    if (lags < 4) return false;

    std::vector<double> x(w);
    double xm = 0.0;
    for (size_t j = 0; j < w; ++j) {
        x[j] = std::sqrt(p.rv[start + j]);  // |log return|
        xm += x[j];
    }
    xm /= static_cast<double>(w);
    for (size_t j = 0; j < w; ++j) x[j] -= xm;

    double denom = 0.0;
    for (size_t j = 0; j < w; ++j) denom += x[j] * x[j];
    if (!(denom > 0.0) || !std::isfinite(denom)) return false;

    // Biased (divide-by-N) estimator at every lag: it is the one that keeps the
    // sequence positive-semidefinite and damps the long lags where the sample
    // is thinnest, which is what a sum-of-exponentials fit needs.
    acf.assign(static_cast<size_t>(lags), 0.0);
    for (int k = 1; k <= lags; ++k) {
        double s = 0.0;
        const size_t lag = static_cast<size_t>(k);
        for (size_t j = 0; j + lag < w; ++j) s += x[j] * x[j + lag];
        acf[static_cast<size_t>(k - 1)] = s / denom;
    }
    return true;
}

std::vector<int> discoverHorizonsAt(const Proxy& p, size_t endBar, int windowBars,
                                    const VolForecastConfig& cfg, int cap) {
    std::vector<int> result;
    std::vector<double> acf;
    if (!absReturnAcf(p, endBar, windowBars, cfg.pencilAcfLags, acf)) return result;

    // modelOrder is left at 0 (let the estimator pick from its singular-value
    // profile) rather than pinned to pencilModes. pencil.h warns that model
    // order is its dominant knob and says to pass it "when you know the
    // order" - but we do not know it, that is the entire question being asked
    // here, and forcing three exponentials onto a series that has one makes
    // the estimator manufacture the other two. Measured on BTC 4h, pinning the
    // order to 3 returns horizon sets like {1, 4, 118} and {1, 5, 95} whose
    // middle term wanders by a factor of 2 between refits, versus a stable
    // single horizon drifting 43 -> 78 when the order is chosen from the data.
    //
    // NOTE on what actually comes back, because it is the headline result of
    // this whole model. Asking for more rates than pencilModes and then
    // selecting a spread across them was tried and changes NOTHING: on BTC 4h,
    // on AAPL daily, and on a GARCH(1,1) calibration, the estimator returns
    // exactly ONE decaying real mode. There is no three-timescale cascade to
    // find in a 750-bar window of these series - the |r| autocorrelation is
    // described by a single dominant exponential, and the further modes the
    // pencil could report are oscillatory or growing and are discarded as
    // unphysical. PencilHAR is therefore a ONE-horizon HAR in practice, and
    // that, not a bug in the discovery, is why it does not beat the
    // three-regressor 1/5/22 model.
    int modes = std::max(1, std::min(3, cfg.pencilModes));
    std::vector<double> lambdas = mathx::decayRates(acf, modes, 0);

    for (double lam : lambdas) {
        if (!std::isfinite(lam) || lam <= 0.0) continue;
        double t = std::round(1.0 / lam);
        if (!std::isfinite(t) || t < 1.0) t = 1.0;
        if (t > cap) t = cap;
        result.push_back(static_cast<int>(t));
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    if (result.size() > 3) result.resize(3);
    return result;
}

// ---------------------------------------------------------------------------
// Causal estimate of the fractional memory parameter d, over bars [.., endBar].
//
// Under fractional integration the autocorrelation decays as a POWER law,
// rho(k) ~ C k^(2d-1), so a straight line through log rho(k) against log k has
// slope 2d-1. That is the estimator used here: OLS on the log-log ACF, which
// is the same quantity `order-spectrum` reports (it measured BTC's volatility
// autocorrelation as k^-0.237, i.e. d = 0.38), so the two numbers are directly
// comparable and a disagreement between them means something.
//
// Two properties make this cheap estimator good enough, and both matter:
//   * The one-bar proxy's sampling noise attenuates every rho(k), k >= 1, by
//     ONE common factor. A constant factor is an intercept in log-log space,
//     so it biases C and leaves the slope - the only thing wanted here -
//     alone. A GPH periodogram regression would need the same argument made
//     about its own low-frequency bias and costs a DFT per refit.
//   * It reuses the autocorrelation PencilHAR already computes, so estimating
//     d costs a 60-point regression on top of work the file was doing anyway.
//     That is what keeps FractionalHAR near HAR's cost instead of the pencil's.
//
// Returns NaN when the fit is degenerate: too few positive rho(k) (the sample
// autocorrelation of a short-memory series crosses zero early and stays
// noise), or a zero-variance design. NaN is a real answer here - it means this
// series does not support the model - and the caller falls back to HAR rather
// than inventing a d.
double estimateFractionalDAt(const Proxy& p, size_t endBar, int windowBars,
                             const VolForecastConfig& cfg) {
    std::vector<double> acf;
    if (!absReturnAcf(p, endBar, windowBars, cfg.pencilAcfLags, acf)) return NaN;

    // Lag 1 is dropped. The power law is an ASYMPTOTIC statement and the first
    // lag is where any short-memory component (bid-ask bounce, a one-bar
    // overreaction) sits on top of it; including it tilts the slope toward
    // whatever the fastest dynamics happen to be.
    //
    // The fitted lags are the longest CONTIGUOUS run of positive rho starting
    // at lag 2 - NOT every lag that happens to be positive. That distinction is
    // the whole correctness of this estimator and it is worth being explicit
    // about, because the obvious version is badly wrong. Taking all positive
    // lags is a selection on the dependent variable: for a series with NO
    // memory, rho(k) is noise of size ~1/sqrt(w) at every lag, so the surviving
    // points have roughly constant magnitude, the log-log slope comes out near
    // zero, and d = (slope+1)/2 lands near 0.5 - the estimator reports maximal
    // long memory for white noise. Measured on an iid control before this was
    // fixed: d = 0.68 full-series, 0.507 rolling mean. With a contiguous run
    // the same control cannot fabricate memory, because a white-noise
    // autocorrelation crosses zero almost immediately (8 consecutive positive
    // lags has probability ~2^-8) and the fit is then refused outright.
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    int n = 0;
    for (size_t k = 2; k <= acf.size(); ++k) {
        double rho = acf[k - 1];
        if (!(rho > 0.0) || !std::isfinite(rho)) break;  // contiguous run ends here
        double lx = std::log(static_cast<double>(k));
        double ly = std::log(rho);
        sx += lx; sy += ly; sxx += lx * lx; sxy += lx * ly;
        ++n;
    }
    if (n < kMinLagsForD) return NaN;

    double den = sxx - sx * sx / n;
    if (!(std::fabs(den) > 1e-12)) return NaN;
    double slope = (sxy - sx * sy / n) / den;
    if (!std::isfinite(slope)) return NaN;
    return 0.5 * (slope + 1.0);  // rho(k) ~ k^(2d-1)
}

// ---------------------------------------------------------------------------
// Refit the regression using ONLY bars [0, atBar].
//
// *** THE H-BAR TARGET BOUNDARY ***
//
// A training row at t pairs regressors ending at bar t with a target averaged
// over bars t+1 .. t+H. The row therefore CONSUMES bar t+H, so the last usable
// row is t = atBar - H, not t = atBar - 1.
//
// Getting that off by one is not a small error, it is the exact shape of the
// bug this whole model is being built to test for. With H = 141 a fit that
// stops at t = atBar-1 trains on 140 rows whose targets run past the refit bar,
// i.e. it fits coefficients on the very future volatility it is then scored on
// predicting. The improvement that produces is entirely fabricated and it
// survives every downstream test, because the coefficients look ordinary and
// only the data they were fitted on is wrong. `futurePerturbation` in the test
// harness exists specifically to catch this: it rewrites every bar after i with
// a violently different path and demands out[i] not move.
//
// Nothing in this function reads an index above atBar.
// ---------------------------------------------------------------------------
Fit refit(const Proxy& p, size_t atBar, const VolForecastConfig& cfg,
          const std::vector<int>& horizons, const std::array<bool, 3>& powerLaw,
          const std::vector<double>& weights, int H, double dHat,
          std::vector<double>& bufX, std::vector<double>& bufY) {
    Fit f;
    if (horizons.empty() || atBar < 1 || H < 1) return f;

    f.nh = static_cast<int>(horizons.size());
    for (int j = 0; j < f.nh; ++j) f.h[static_cast<size_t>(j)] = horizons[static_cast<size_t>(j)];
    f.powerLaw = powerLaw;
    f.weights = weights;
    f.horizon = H;
    f.dHat = dHat;
    f.maxH = horizons.back();
    for (int j = 0; j < f.nh; ++j)
        if (f.powerLaw[static_cast<size_t>(j)] &&
            f.weights.size() != static_cast<size_t>(f.h[static_cast<size_t>(j)])) return f;

    if (atBar < static_cast<size_t>(H)) return f;
    const size_t tEnd = atBar - static_cast<size_t>(H);  // last regressor bar (see above)
    if (tEnd < static_cast<size_t>(f.maxH)) return f;    // horizon not warm yet
    size_t tStart = static_cast<size_t>(f.maxH);
    if (cfg.fitWindow > 0 && tEnd + 1 > static_cast<size_t>(cfg.fitWindow))
        tStart = std::max(tStart, tEnd + 1 - static_cast<size_t>(cfg.fitWindow));

    // eps is fixed once per fit, from the mean variance over the training span,
    // and is then used identically at fit time and at prediction time. A
    // transform whose constants drift between the two would make the predictor
    // inconsistent with the coefficients it is using.
    size_t spanFrom = tStart + 1 - static_cast<size_t>(f.maxH);
    if (spanFrom < 1) spanFrom = 1;
    double mean = 0.0;
    size_t cnt = 0;
    for (size_t t = spanFrom; t <= atBar; ++t) {
        if (p.badPrefix[t + 1] != p.badPrefix[t]) continue;
        mean += p.rv[t];
        ++cnt;
    }
    if (cnt == 0) return f;
    mean /= static_cast<double>(cnt);
    if (!(mean > 0.0) || !std::isfinite(mean)) return f;
    f.eps = kEpsFrac * mean;

    // Pass 1: materialize the design. Logs are the expensive part of this
    // routine, so they are paid once and the later passes read buffers.
    const int nh = f.nh;
    bufX.clear();
    bufY.clear();
    for (size_t t = tStart; t <= tEnd; ++t) {
        // Regressors span [t-maxH+1, t]; the target spans [t+1, t+H]. All of it
        // must be free of unusable bars, or the sample is silently a different
        // quantity than the one being modelled.
        if (!spanOk(p, t + 1 - static_cast<size_t>(f.maxH), t + static_cast<size_t>(H))) continue;
        for (int j = 0; j < nh; ++j)
            bufX.push_back(std::log(regressorValue(p, t, f, j) + f.eps));
        bufY.push_back(std::log(avgRvForward(p, t, H) + f.eps));
    }
    const size_t m = bufY.size();
    if (m < static_cast<size_t>(std::max(nh + 1, cfg.minFitSamples))) return f;

    // Centering the design removes the intercept column, which both improves
    // the conditioning (the regressors live around log(variance) ~ -8, so an
    // uncentered normal matrix is dominated by that offset) and lets the
    // intercept absorb the -1.27 mean of the log-chi-square target noise
    // without it ever appearing as a modelling assumption.
    double mY = 0.0;
    for (size_t i = 0; i < m; ++i) mY += bufY[i];
    mY /= static_cast<double>(m);
    double mX[3] = {0.0, 0.0, 0.0};
    for (size_t i = 0; i < m; ++i)
        for (int j = 0; j < nh; ++j) mX[j] += bufX[i * static_cast<size_t>(nh) + static_cast<size_t>(j)];
    for (int j = 0; j < nh; ++j) mX[j] /= static_cast<double>(m);

    double xtx[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    double xty[3] = {0.0, 0.0, 0.0};
    for (size_t i = 0; i < m; ++i) {
        double c[3];
        for (int j = 0; j < nh; ++j)
            c[j] = bufX[i * static_cast<size_t>(nh) + static_cast<size_t>(j)] - mX[j];
        double dy = bufY[i] - mY;
        for (int a = 0; a < nh; ++a) {
            xty[a] += c[a] * dy;
            for (int b = a; b < nh; ++b) xtx[a][b] += c[a] * c[b];
        }
    }
    for (int a = 0; a < nh; ++a)
        for (int b = 0; b < a; ++b) xtx[a][b] = xtx[b][a];

    double beta[3] = {0.0, 0.0, 0.0};
    if (!solveSmall(xtx, xty, nh, beta)) return f;  // singular -> caller falls back to Trailing
    for (int j = 0; j < nh; ++j)
        if (!std::isfinite(beta[j])) return f;

    // Pass 3: residuals give the Duan smearing factor and the extrapolation
    // clamp. Both are properties of the fit window, i.e. of data <= atBar.
    double smearSum = 0.0;
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < m; ++i) {
        double yhat = mY;
        for (int j = 0; j < nh; ++j)
            yhat += beta[j] * (bufX[i * static_cast<size_t>(nh) + static_cast<size_t>(j)] - mX[j]);
        lo = std::min(lo, yhat);
        hi = std::max(hi, yhat);
        smearSum += std::exp(bufY[i] - yhat);
    }
    double smear = smearSum / static_cast<double>(m);
    if (!std::isfinite(smear) || smear <= 0.0 || smear > kMaxSmear) return f;
    if (!std::isfinite(lo) || !std::isfinite(hi)) return f;

    f.meanY = mY;
    for (int j = 0; j < nh; ++j) {
        f.meanX[static_cast<size_t>(j)] = mX[j];
        f.beta[static_cast<size_t>(j)] = beta[j];
    }
    f.smear = smear;
    f.lo = lo - kClampSlack;
    f.hi = hi + kClampSlack;
    f.valid = true;
    return f;
}

// Forecast volatility at bar i from an already-fitted model. NaN means "this
// fit cannot speak about this bar"; the caller decides the fallback.
//
// The regressors are backward-looking, so this reads only bars <= i regardless
// of H: the horizon lives entirely in what the coefficients were TRAINED to
// predict, never in what the predictor reads.
double predict(const Proxy& p, size_t i, const Fit& f) {
    if (!f.valid) return NaN;
    if (i < static_cast<size_t>(f.maxH)) return NaN;
    if (!spanOk(p, i + 1 - static_cast<size_t>(f.maxH), i)) return NaN;

    double yhat = f.meanY;
    for (int j = 0; j < f.nh; ++j) {
        double x = std::log(regressorValue(p, i, f, j) + f.eps);
        yhat += f.beta[static_cast<size_t>(j)] * (x - f.meanX[static_cast<size_t>(j)]);
    }
    if (!std::isfinite(yhat)) return NaN;
    yhat = std::min(std::max(yhat, f.lo), f.hi);

    double variance = f.smear * std::exp(yhat);
    if (!std::isfinite(variance) || variance <= 0.0) return NaN;
    double sd = std::sqrt(variance);
    return std::isfinite(sd) ? sd : NaN;
}

} // namespace

std::vector<double> forecastVolatility(const std::vector<double>& close,
                                       const VolForecastConfig& cfg) {
    const size_t n = close.size();
    std::vector<double> out(n, NaN);
    if (n < 2) return out;

    // Computed unconditionally: it IS the Trailing model, and it is the
    // documented fallback for the regression models. Calling the existing
    // indicator rather than reimplementing the same formula is deliberate -
    // it is what makes VolModel::Trailing bit-identical to what the engine
    // does today, which is the only way the A/B measures one variable.
    const std::vector<double> trailing =
        rollingVolatility(close, std::max(2, cfg.trailingWindow));
    if (cfg.model == VolModel::Trailing) return trailing;

    const Proxy p = buildProxy(close);
    const int cap = horizonCap(cfg);
    const std::vector<int> harHorizons = sanitizeHorizons(cfg.harWindows, cap);
    const int refitEvery = std::max(1, cfg.refitEvery);
    const int H = std::max(1, cfg.horizonBars);
    // At H=1 this is minFitSamples+1, exactly the pre-horizon value, so the
    // attempt schedule - and therefore every result - is unchanged. For H>1 it
    // just skips attempts that cannot possibly have enough rows, since each
    // training row now consumes H bars instead of 1.
    const size_t minStart = static_cast<size_t>(std::max(1, cfg.minFitSamples)) +
                            static_cast<size_t>(H);

    // The refit schedule is a pure function of the bar INDEX (first attempt at
    // minStart, then every refitEvery bars) and never of the series length or
    // of whether earlier fits succeeded. That is what makes the truncation test
    // - recompute from close[0..i], demand the same out[i] - pass exactly
    // rather than approximately.
    std::vector<double> bufX, bufY;
    bufX.reserve(static_cast<size_t>(std::max(1, cfg.fitWindow)) * 3);
    bufY.reserve(static_cast<size_t>(std::max(1, cfg.fitWindow)));

    Fit fit;
    bool warm = false;  // has any fit ever succeeded? before that: NaN, per contract
    long long lastAttempt = -1;
    std::vector<double> weights;

    for (size_t i = 0; i < n; ++i) {
        if (i >= minStart &&
            (lastAttempt < 0 || static_cast<long long>(i) - lastAttempt >= refitEvery)) {
            lastAttempt = static_cast<long long>(i);
            std::vector<int> horizons = harHorizons;
            std::array<bool, 3> powerLaw{{false, false, false}};
            double dUsed = 0.0;
            weights.clear();

            if (cfg.model == VolModel::PencilHAR) {
                // Rediscovered on every refit, from the window ending at i.
                // A horizon set discovered once on the whole series and reused
                // would be a look-ahead even though no price is read twice.
                horizons = discoverHorizonsAt(p, i, cfg.fitWindow, cfg, cap);
                if (horizons.empty()) horizons = harHorizons;  // fall back to Corsi's 1/5/22
            } else if (cfg.model == VolModel::FractionalHAR) {
                // d is re-estimated from the window ending at i, on the same
                // refit schedule as everything else. Pinning it via
                // cfg.fractionalD skips the estimate entirely.
                double d = cfg.fractionalD;
                if (!(d > 0.0)) d = estimateFractionalDAt(p, i, cfg.fitWindow, cfg);
                if (std::isfinite(d) && d > kDMax) d = kDMax;  // shape still right at the limit

                // Degenerate or short-memory estimate -> plain HAR. Falling back
                // to a d of 0 would hand a short-memory series the longest
                // regressor in the file (psi_j ~ 1/j); falling back to Corsi's
                // buckets is the conservative answer and it is also the exact
                // model this one is being A/B'd against.
                if (std::isfinite(d) && d >= kDMin) {
                    // Keep harWindows[0] and [1] as flat short buckets and
                    // replace only the long one. Over a handful of bars a power
                    // law and a flat average are indistinguishable in shape but
                    // the flat version is less noisy, so there is nothing to win
                    // at the short end and the long end is where all the
                    // disagreement between the models lives.
                    buildPowerLawWeights(d, cap, weights);
                    if (!weights.empty()) {
                        std::array<int, 3> spans{{cfg.harWindows[0], cfg.harWindows[1], cap}};
                        horizons = sanitizeHorizons(spans, cap);
                        // sanitizeHorizons sorts ascending and dedups, so the
                        // power-law term is whichever entry ended up at `cap`.
                        for (size_t j = 0; j < horizons.size(); ++j)
                            powerLaw[j] = (horizons[j] == cap);
                        dUsed = d;
                    }
                }
            }
            fit = refit(p, i, cfg, horizons, powerLaw, weights, H, dUsed, bufX, bufY);
            if (fit.valid) warm = true;
        }

        if (!warm) continue;  // NaN: fewer than minFitSamples usable samples so far
        double v = predict(p, i, fit);
        out[i] = std::isfinite(v) && v > 0.0 ? v : trailing[i];
    }
    return out;
}

std::array<int, 3> discoveredHorizons(const std::vector<double>& close,
                                      const VolForecastConfig& cfg) {
    std::array<int, 3> out{{0, 0, 0}};
    if (cfg.model != VolModel::PencilHAR || close.size() < 2) return out;
    const Proxy p = buildProxy(close);
    // Whole series on purpose: this is the reporting entry point, and it is
    // never consulted by forecastVolatility().
    std::vector<int> h = discoverHorizonsAt(p, close.size() - 1,
                                            static_cast<int>(close.size()), cfg,
                                            horizonCap(cfg));
    for (size_t j = 0; j < h.size() && j < 3; ++j) out[j] = h[j];
    return out;
}

double estimatedMemoryD(const std::vector<double>& close, const VolForecastConfig& cfg) {
    if (close.size() < 2) return NaN;
    const Proxy p = buildProxy(close);
    // Whole series on purpose - reporting entry point, like discoveredHorizons.
    // Call it on a prefix to recover the causal value used at that bar.
    return estimateFractionalDAt(p, close.size() - 1, static_cast<int>(close.size()), cfg);
}

} // namespace trader::indicators
