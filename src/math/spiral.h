#pragma once
#include "pencil.h"
#include <complex>
#include <vector>

// Logarithmic-spiral order estimator: the headline method of
//
//   U. Merlan, "Identification of the order distribution of linear systems by
//   line-spectrum estimation on logarithmic spirals".
//
// Given TIME samples y(t) of a system's response, the truncated Laplace
// transform
//
//     Yhat_A(s) = integral_0^A y(tau) exp(-s tau) dtau
//
// is evaluated on a logarithmic spiral s = s0 * exp((sigma + i) u). If Yhat is
// an "order comb" sum_m c_m s^{alpha_m}, its samples along the spiral form an
// exponential sum in the sample index with nodes z_m = exp((sigma+i) du
// alpha_m), so the orders follow from a matrix-pencil line-spectrum estimate.
//
// WHY A SPIRAL AND NOT A CIRCLE. On the unit circle s = exp(i u) the transform
// of time-domain data is exactly 2*pi-periodic, so its inverse is supported on
// the integers: non-integer orders can NEVER appear, for any window, sampling
// rate or FFT length. That is a theorem about the readout, not about the data.
// Any sigma != 0 breaks the periodicity and makes log s injective along the
// contour, which is what makes a fractional order localizable at all.
//
// WHAT THE ORDER MEANS HERE. Applied to a decaying autocorrelation, the
// recovered alpha says which model class the memory belongs to:
//     alpha ~ -1   integer order: exponential relaxation, ordinary ARMA memory
//     -1 < alpha < 0  fractional order: power-law (long) memory, ACF ~ k^-(alpha+1)
//
// THE RADIUS IS NOT COSMETIC. The paper fixes the contour near |s| = 1, which
// is right for its purpose: for a pure order comb the radius only rescales
// amplitudes, because s^alpha is scale-invariant. For DISCRIMINATING model
// classes it is the central knob, because a Laplace contour at |s| interrogates
// timescales t ~ 1/|s|, and exponential and power-law memory are
// indistinguishable at short t. Measured on synthetic controls: a true k^-0.3
// autocorrelation returns alpha = -0.963 at radius 1 (wrong; truth is -0.70)
// and -0.705 at radius 0.01 (right). So a single radius reports a number that
// may mean nothing; a SWEEP is the measurement.
//
// THE STABILITY OF ALPHA ACROSS RADII IS THE REAL STATISTIC. A genuine
// fractional order is a radius-independent constant once the contour reaches
// the band where the power law lives. Synthetic power laws settle to within
// 0.01-0.05 across a decade of radii; the volatility autocorrelations of the
// instruments in this repo wander by ~0.5, which is the whole distance between
// "exponential" and "fractional". Report the spread, not just the value.
namespace trader::mathx {

struct SpiralConfig {
    double radius = 1.0;        // s0: contour scale, selects timescale t ~ 1/s0
    double sigma = 0.3;         // spiral tightness; sigma != 0 is what breaks
                                // the circle's periodicity (see above)
    double arcHalfWidth = 1.35; // u in [-arc, +arc]; must stay inside
                                // (-pi/2, pi/2) so Re(s) > 0 and the truncated
                                // transform converges
    int    points = 64;         // samples along the arc
    int    modelOrder = 1;      // pencil model order; 1 = "one dominant order"
};

// READING ALPHA FROM THE MODE. The spiral model says omega = (sigma + i) *
// alpha with alpha REAL, so Re(omega) = sigma*alpha and Im(omega) = alpha are
// two independent readings of the same order. Reading from Re alone divides
// by sigma (= 0.3 by default), amplifying estimation noise 1/sigma-fold;
// alpha is therefore taken from the least-squares projection of omega onto
// the model direction (sigma, 1):
//     alpha = (sigma*Re(omega) + Im(omega)) / (1 + sigma^2).
// Measured on this repo's sweep of 98 (instrument, timeframe) stores, the
// projection readout's identification ratio rank-predicts tsmom excess
// Sharpe better than the Re readout (Spearman -0.263 vs -0.231, perm
// p = 0.031 vs 0.059) and its radius stability on BTC tightens ~40x.
//
// The orthogonal component
//     beta = (sigma*Im(omega) - Re(omega)) / (1 + sigma^2)
// is the COMPLEX part of the order: a genuinely real order has beta ~ 0
// (synthetic power laws: |beta| <= 0.003 across a decade of radii), while
// beta != 0 means log-periodic modulation of the power law - discrete scale
// invariance, ACF ~ k^-(alpha+1) * cos(beta ln k + phase) - and exponentials
// leak |beta| up to ~0.04. So beta is both a model-consistency check and a
// DSI detector.
struct SpiralResult {
    bool   ok = false;
    double alpha = 0.0;      // leading order, projection readout (see above)
    double beta = 0.0;       // complex-order component; ~0 for a real order
    double amplitude = 0.0;
    double minReS = 0.0;     // smallest Re(s) on the arc; must be > 0
    std::vector<PencilMode> modes;
};

// Truncated Laplace transform of uniformly sampled y (spacing dt, starting at
// t = 0) evaluated at each s, by composite Simpson quadrature.
std::vector<std::complex<double>> truncatedLaplace(const std::vector<double>& y,
                                                    double dt,
                                                    const std::vector<std::complex<double>>& s);

// Normalized autocorrelation of a series out to `maxLag` (clamped to n/4).
// This is the input spiralOrder expects: applied to |log returns| it measures
// volatility memory, applied to log returns themselves, return memory.
// Returns {} if the series is too short or has zero variance.
std::vector<double> autocorrelation(const std::vector<double>& series, size_t maxLag);

// The spiral contour itself, plus the sample spacing du used to convert nodes
// back into orders.
std::vector<std::complex<double>> spiralContour(const SpiralConfig& cfg, double& duOut);

// One estimate at one radius. Never throws; ok == false on breakdown.
SpiralResult spiralOrder(const std::vector<double>& y, double dt, const SpiralConfig& cfg);

} // namespace trader::mathx
