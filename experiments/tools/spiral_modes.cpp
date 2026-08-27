// spiral_modes - score one candle store's order spectrum and emit the RAW
// COMPLEX PENCIL MODES as JSON, instead of a readout derived from them.
//
// WHY THIS EXISTS RATHER THAN PARSING `cli_trader order-spectrum`. That
// command prints alpha - one number per (series, radius), already collapsed
// out of the complex mode by whichever readout src/math/spiral.cpp currently
// ships. The experiment's whole question was WHICH READOUT to ship, and you
// cannot answer that from the output of one of the candidates: recovering
// Im(omega) from a printed alpha is impossible, so every new readout meant
// re-running the estimator over all 98 stores. Emitting omega itself makes
// the expensive part - the O(n * maxLag) autocorrelation and five pencil fits
// per series - a single run whose output supports ANY readout computed
// downstream:
//
//     alpha_Re   = Re(omega) / sigma                      (the older readout)
//     alpha_Im   = Im(omega)
//     alpha_proj = (sigma*Re + Im) / (1 + sigma^2)        (what now ships)
//     beta       = (sigma*Im - Re) / (1 + sigma^2)        (complex component)
//
// That is what made the readout head-to-head PRE-REGISTERABLE. Both readouts
// were computed from one frozen set of modes, so neither could be quietly
// re-run with different settings after seeing its score, and the comparison
// was arithmetic on saved numbers rather than a second measurement.
//
// WHY IT REIMPLEMENTS THE CONSTRUCTION INSTEAD OF CALLING cmdOrderSpectrum.
// cmdOrderSpectrum is a CLI command that prints a report; its construction is
// not exposed as a library function. This file therefore replicates it
// EXACTLY, and "exactly" is the requirement, not an aspiration - a mismatch
// anywhere would make the experiment's conclusions apply to an estimator the
// repo does not ship:
//   * signal        = |log(close[i]/close[i-1])|, bars with a non-positive
//                     close on either side dropped (the volatility proxy;
//                     `--input volatility`, the default)
//   * autocorrelation = BIASED, normalized by the full-sample variance, i.e.
//                     acf[k] = sum_i (v_i - m)(v_{i+k} - m) / sum_i (v_i - m)^2
//                     with the sum running only while i+k < n. The shrinking
//                     overlap at large k is deliberate: it is what makes the
//                     estimate taper rather than blow up in the tail, and the
//                     unbiased version is NOT interchangeable here.
//   * maxLag        = min(2000, n/4), matching autocorrelation()'s clamp
//   * references    = the same four synthetics the command computes, at the
//                     SAME lag count: power laws k^-0.3 and k^-0.6
//                     (fractional), exp(-k/20) and exp(-k/200) (integer)
//   * contour       = SpiralConfig defaults: sigma 0.3, arc 1.35, 64 points,
//                     model order 1
//   * radii         = 1, 0.3, 0.1, 0.03, 0.01, and dt = 1 bar
//
// The references are not optional decoration. The estimator returns an order
// for ANY input, so a measured alpha means nothing without knowing what the
// same machinery returns at the same lag count for memory that is known to be
// power-law and memory that is known to be exponential. They are recomputed
// per store because their lag count follows that store's n.
//
// WHICH MODE IS EMITTED. matrixPencil returns modes sorted by descending
// amplitude and this runs at model order 1, so the leading mode IS the fit;
// modes.front() is the same mode src/math/spiral.cpp reads alpha from. A
// radius where the fit broke down emits `null` rather than being skipped, so
// the array index always identifies the radius.
//
// Output: exactly one JSON object on stdout, and nothing else - diagnostics
// go to stderr so the caller can pipe stdout straight into a JSON parser.
#include "core/candle_store.h"
#include "math/spiral.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace trader;

// The contour radii of `cli_trader order-spectrum`'s default sweep, largest
// first. A single radius is not a measurement (see src/math/spiral.h); the
// statistic is how alpha behaves as the contour moves out to long timescales,
// which is why the downstream readouts average over the SMALLEST three.
static const std::vector<double> kRadii = {1.0, 0.3, 0.1, 0.03, 0.01};

// Fit at every radius and emit the leading mode as [Re, Im] pairs.
static void emitSeries(const char* name, const std::vector<double>& y, bool& firstSeries) {
    printf("%s\"%s\":[", firstSeries ? "" : ",", name);
    firstSeries = false;
    bool first = true;
    for (double r : kRadii) {
        mathx::SpiralConfig cfg;   // sigma 0.3, arc 1.35, 64 points, order 1
        cfg.radius = r;
        auto res = mathx::spiralOrder(y, /*dt=*/1.0, cfg);
        if (!first) printf(",");
        first = false;
        if (res.ok && !res.modes.empty()) {
            auto w = res.modes.front().omega;
            printf("[%.10g,%.10g]", w.real(), w.imag());
        } else {
            printf("null");
        }
    }
    printf("]");
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: spiral_modes <store.ctc>\n"
                        "emits JSON: the leading complex pencil mode per radius, for the\n"
                        "store's volatility autocorrelation and the four reference series.\n");
        return 1;
    }
    CandleStore store(argv[1]);
    if (!store.isValidStore()) { fprintf(stderr, "invalid store: %s\n", argv[1]); return 1; }
    CandleSeries s = store.load();
    if (s.size() < 200) { fprintf(stderr, "too few candles\n"); return 1; }

    std::vector<double> vol;
    vol.reserve(s.size());
    for (size_t i = 1; i < s.size(); ++i)
        if (s.close[i] > 0.0 && s.close[i - 1] > 0.0)
            vol.push_back(std::fabs(std::log(s.close[i] / s.close[i - 1])));

    size_t n = vol.size();
    size_t K = std::min<size_t>(2000, n / 4);
    double mean = 0.0;
    for (double v : vol) mean += v;
    mean /= static_cast<double>(n);
    double var = 0.0;
    for (double v : vol) var += (v - mean) * (v - mean);
    if (var <= 0.0) { fprintf(stderr, "degenerate series\n"); return 1; }
    std::vector<double> acf(K + 1, 0.0);
    for (size_t k = 0; k <= K; ++k) {
        double sum = 0.0;
        for (size_t i = 0; i + k < n; ++i) sum += (vol[i] - mean) * (vol[i + k] - mean);
        acf[k] = sum / var;
    }

    // References at the store's own lag count, as the command computes them.
    std::vector<double> pl03(K + 1, 1.0), pl06(K + 1, 1.0), exp20(K + 1), exp200(K + 1);
    for (size_t k = 1; k <= K; ++k) {
        pl03[k] = std::pow(static_cast<double>(k), -0.3);
        pl06[k] = std::pow(static_cast<double>(k), -0.6);
    }
    for (size_t k = 0; k <= K; ++k) {
        exp20[k] = std::exp(-static_cast<double>(k) / 20.0);
        exp200[k] = std::exp(-static_cast<double>(k) / 200.0);
    }

    // sigma is echoed so a consumer can apply a readout without hardcoding
    // the contour it was measured on; it is SpiralConfig's default, and the
    // radii come from the same vector the sweep above used, so the metadata
    // cannot drift away from the numbers.
    mathx::SpiralConfig defaults;
    printf("{\"symbol\":\"%s\",\"period\":%lld,\"candles\":%zu,\"lags\":%zu,\"sigma\":%.10g,"
           "\"radii\":[",
           s.symbol.c_str(), static_cast<long long>(s.periodSeconds), s.size(), K,
           defaults.sigma);
    for (size_t i = 0; i < kRadii.size(); ++i) printf("%s%.10g", i ? "," : "", kRadii[i]);
    printf("],\"modes\":{");
    bool first = true;
    emitSeries("measured", acf, first);
    emitSeries("pl03", pl03, first);
    emitSeries("pl06", pl06, first);
    emitSeries("exp20", exp20, first);
    emitSeries("exp200", exp200, first);
    printf("}}\n");
    return 0;
}
