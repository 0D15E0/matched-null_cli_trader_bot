#pragma once
#include <complex>
#include <vector>

// Matrix-pencil (Hua-Sarkar) estimator for finite sums of complex
// exponentials.
//
// This is the "spiral line-spectrum" estimator from the repo owner's own
// paper - U. Merlan, "Identification of the order distribution of linear
// systems by line-spectrum estimation on logarithmic spirals", Section 4
// (spiral estimator) and Section 5.5 / E5 (log-sampled measures) - which is
// the classical algorithm of
//
//   Y. Hua and T. K. Sarkar, "Matrix pencil method for estimating parameters
//   of exponentially damped/undamped sinusoids in noise", IEEE Trans. Acoust.
//   Speech Signal Process. 38 (1990) 814-824.
//
// MODEL. Given uniformly spaced samples of
//
//     y[j] = sum_m c_m * z_m^j ,     z_m = exp(omega_m * delta),
//
// the estimator recovers the complex exponents omega_m and amplitudes c_m.
// Re(omega) is a damping/growth rate (single-valued - it comes from |z|, so
// there is no branch ambiguity). Im(omega) = arg(z)/delta is a frequency and
// is only defined modulo 2*pi/delta, so anything with |Im(omega)| >= pi/delta
// is aliased and must be discarded by the caller.
//
// WHY IT IS IN A TRADING REPO. Two uses:
//   * log-periodicity. A measure sampled uniformly in lambda = log(x) has
//     mode expansion sum_k c_k exp(omega_k lambda); a nonzero Im(omega) is
//     direct evidence of discrete scale invariance with preferred ratio
//     exp(2*pi/Im(omega)). That is the same structure LPPL-style crash
//     signatures assume, estimated here without fitting a nonlinear model.
//   * volatility cascades. The autocorrelation of |return| is a sum of
//     decaying exponentials whose rates are the cascade timescales;
//     decayRates() below extracts them.
//
// IMPLEMENTATION NOTES (all linear algebra is local - the project links only
// libcurl/OpenSSL/nlohmann, so there is no Eigen/LAPACK to lean on):
//   * the SVD uses a ONE-SIDED (Hestenes) Jacobi iteration applied to the
//     Hankel matrix directly, never to Y^H Y. Forming the Gram matrix squares
//     the condition number and on the Cantor calibration below it loses the
//     smallest retained singular value entirely (s_22/s_0 ~ 9e-10, squared
//     that is below the double-precision epsilon), which degrades the highest
//     recovered modes from ~1e-8 to ~5e-6. One-sided Jacobi keeps them.
//   * the M x M eigenproblem is solved by unitary Hessenberg reduction plus a
//     shifted complex QR iteration, NOT by characteristic-polynomial root
//     finding: the model orders this estimator actually needs (M = 23 on the
//     calibration case) are far past the point where Faddeev-LeVerrier
//     coefficients are meaningful. A Faddeev-LeVerrier + Durand-Kerner path
//     is retained only as a last-ditch fallback if QR fails to converge.
//
// CAVEATS a caller must know:
//   * MODEL ORDER IS THE DOMINANT KNOB. The estimator is far more sensitive
//     to M than to anything else. On the Cantor calibration, M = 23 recovers
//     the top eight modes to 1e-12...1e-8 while M = 15 gets the eighth mode
//     wrong in the second decimal. When you know the order, pass it.
//   * NOISE DOMINATES EVERYTHING ELSE. The near-machine-precision numbers
//     that noiseless calibrations produce are an exact-arithmetic artifact.
//     Relative noise eta degrades the exponents roughly like eta^(1/(2k+1))
//     for the k-th mode: 1e-8 relative noise on a two-mode fit costs ~1e-7 in
//     the exponent, but the same noise on the eight-mode Cantor case costs
//     O(0.1) in the weakest modes.
//   * minimum N is 4; useful N is >= 4*M. delta must be nonzero and finite.
//   * amplitudes are found from a Vandermonde least squares, which overflows
//     if max|z|^N does; the fit is abandoned (empty result) if that happens.
namespace trader::mathx {

struct PencilMode {
    std::complex<double> omega;      // exponent: value/delta space
    std::complex<double> amplitude;  // c_m
    double magnitude() const;        // |amplitude|
};

struct PencilOptions {
    int  modelOrder  = 0;   // M; <=0 means choose from the singular-value profile
    int  pencilParam = -1;  // L; <0 means N/2
    double energyThreshold = 1e-8; // used only when modelOrder <= 0
    int  maxModelOrder = 8;
};

// Fit y[j] = sum_m c_m exp(omega_m * delta * j). Returns modes sorted by
// descending |amplitude|. Empty on failure (too few samples, non-finite
// input, or numerical breakdown) - never throws.
std::vector<PencilMode> matrixPencil(const std::vector<double>& y,
                                      double delta,
                                      const PencilOptions& opts = {});

// Same estimator for genuinely complex data. The real overload above simply
// forwards here. This overload exists because the real case cannot see one
// whole class of bug: for real y the true node set is closed under
// conjugation, so an implementation that returns conj(z) instead of z is
// indistinguishable from a correct one (it just relabels the pair). Only
// non-conjugate-symmetric complex input pins the convention down, and the
// amplitudes are wrong - not merely relabelled - when it is broken.
std::vector<PencilMode> matrixPencil(const std::vector<std::complex<double>>& y,
                                      double delta,
                                      const PencilOptions& opts = {});

// Convenience: fit a decaying-exponential model to an autocorrelation-like
// sequence and return the positive real decay RATES lambda_m (so that the
// mode behaves like exp(-lambda_m * k)), sorted ascending (slowest decay
// first), discarding oscillatory or growing modes. Used for discovering
// volatility cascade timescales. Returns at most `maxRates` values.
std::vector<double> decayRates(const std::vector<double>& acf,
                                int maxRates = 3,
                                int modelOrder = 0);

} // namespace trader::mathx
