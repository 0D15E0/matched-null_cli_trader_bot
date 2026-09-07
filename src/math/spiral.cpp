#include "spiral.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace trader::mathx {

std::vector<double> autocorrelation(const std::vector<double>& series, size_t maxLag) {
    size_t n = series.size();
    if (n < 4) return {};
    maxLag = std::min(maxLag, n / 4);
    double mean = 0.0;
    for (double v : series) mean += v;
    mean /= static_cast<double>(n);
    double var = 0.0;
    for (double v : series) var += (v - mean) * (v - mean);
    if (var <= 0.0) return {};
    std::vector<double> acf(maxLag + 1, 0.0);
    for (size_t k = 0; k <= maxLag; ++k) {
        double acc = 0.0;
        for (size_t i = 0; i + k < n; ++i) acc += (series[i] - mean) * (series[i + k] - mean);
        acf[k] = acc / var;
    }
    return acf;
}


std::vector<std::complex<double>> spiralContour(const SpiralConfig& cfg, double& duOut) {
    std::vector<std::complex<double>> s;
    int n = std::max(4, cfg.points);
    double u0 = -std::fabs(cfg.arcHalfWidth);
    double u1 = std::fabs(cfg.arcHalfWidth);
    duOut = (u1 - u0) / static_cast<double>(n - 1);
    s.reserve(static_cast<size_t>(n));
    for (int j = 0; j < n; ++j) {
        double u = u0 + duOut * j;
        s.push_back(cfg.radius * std::exp(std::complex<double>(cfg.sigma, 1.0) * u));
    }
    return s;
}

std::vector<std::complex<double>> truncatedLaplace(const std::vector<double>& y, double dt,
                                                    const std::vector<std::complex<double>>& s) {
    std::vector<std::complex<double>> out(s.size(), std::complex<double>(0.0, 0.0));
    size_t n = y.size();
    if (n < 3 || dt <= 0.0) return out;
    // Composite Simpson needs an odd number of points; drop the last sample
    // rather than silently mixing rules.
    if (n % 2 == 0) --n;

    std::vector<double> w(n, 1.0);
    for (size_t i = 1; i + 1 < n; ++i) w[i] = (i % 2 == 1) ? 4.0 : 2.0;
    for (double& v : w) v *= dt / 3.0;

    for (size_t k = 0; k < s.size(); ++k) {
        std::complex<double> acc(0.0, 0.0);
        for (size_t i = 0; i < n; ++i) {
            double t = dt * static_cast<double>(i);
            acc += std::exp(-s[k] * t) * (y[i] * w[i]);
        }
        out[k] = acc;
    }
    return out;
}

SpiralResult spiralOrder(const std::vector<double>& y, double dt, const SpiralConfig& cfg) {
    SpiralResult r;
    if (y.size() < 8 || dt <= 0.0 || cfg.sigma == 0.0 || cfg.radius <= 0.0) return r;
    for (double v : y) if (!std::isfinite(v)) return r;

    double du = 0.0;
    auto s = spiralContour(cfg, du);
    if (du <= 0.0) return r;

    r.minReS = std::numeric_limits<double>::infinity();
    for (const auto& sv : s) r.minReS = std::min(r.minReS, sv.real());
    // A contour touching or crossing the imaginary axis makes the truncated
    // transform meaningless for a non-decaying signal; refuse rather than
    // return a number.
    if (!(r.minReS > 0.0)) return r;

    auto F = truncatedLaplace(y, dt, s);
    for (const auto& v : F) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) return r;

    PencilOptions opts;
    opts.modelOrder = cfg.modelOrder;
    auto modes = matrixPencil(F, du, opts);
    if (modes.empty()) return r;

    // Proposition 4.2: the order comes from the node, which is single-valued
    // along the spiral - there is no branch condition to satisfy, which is
    // precisely the degree of freedom the unit circle lacks.
    // The pencil reports omega with z = exp(omega * du); the spiral model has
    // z = exp((sigma + i) du alpha) with alpha REAL, so omega = (sigma + i)
    // alpha. Both Re(omega)/sigma and Im(omega) read alpha; take the
    // least-squares projection onto the model direction (sigma, 1), and keep
    // the perpendicular component as beta - the complex-order (log-periodic)
    // content, ~0 when the order is genuinely real. See spiral.h.
    r.modes = modes;
    const PencilMode& lead = modes.front();          // already sorted by |c|
    const double s2 = 1.0 + cfg.sigma * cfg.sigma;
    r.alpha = (cfg.sigma * lead.omega.real() + lead.omega.imag()) / s2;
    r.beta = (cfg.sigma * lead.omega.imag() - lead.omega.real()) / s2;
    r.amplitude = lead.magnitude();
    r.ok = std::isfinite(r.alpha) && std::isfinite(r.beta);
    return r;
}

} // namespace trader::mathx
