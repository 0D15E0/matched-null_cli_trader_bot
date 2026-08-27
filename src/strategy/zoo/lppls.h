#pragma once
#include "../strategy.h"
#include "zoo_common.h"
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace trader {

// ---------------------------------------------------------------------------
// Log-Periodic Power Law Singularity (LPPLS) bubble detection.
//
// Sornette's model of a speculative bubble (Johansen, Ledoit & Sornette 2000,
// International Journal of Theoretical and Applied Finance 3(2); Filimonov &
// Sornette 2013, Physica A 392) says a bubble is not merely a fast rise but a
// FASTER-THAN-EXPONENTIAL rise, decorated with accelerating oscillations,
// terminating at a critical time t_c:
//
//     ln p(t) = A + B (t_c - t)^m + C (t_c - t)^m cos(omega ln(t_c - t) - phi)
//
// The power law with 0 < m < 1 is the super-exponential part: positive
// feedback between traders makes the growth rate itself grow, and the price
// reaches a finite value in finite time with infinite slope. The log-periodic
// term is the signature of DISCRETE scale invariance - the oscillations are
// evenly spaced in log(t_c - t), so they compress as the critical time
// approaches. That log-periodicity is the falsifiable part of the theory and
// the reason it is not just a curve fit: a merely fast rise has no reason to
// oscillate on a geometric schedule.
//
// This repo already measures the same phenomenon from the other direction -
// `order-spectrum` reports a nonzero complex order beta as evidence of
// discrete scale invariance in a series' memory. This strategy asks whether
// the same structure, detected in the price path, is worth trading.
//
// Fitting follows Filimonov & Sornette's reduced formulation: expanding
// C cos(omega ln tau - phi) into C1 cos + C2 sin removes the phase from the
// nonlinear parameters, leaving (A, B, C1, C2) LINEAR given (t_c, m, omega).
// So the fit is a coarse grid over three nonlinear parameters, each with a
// closed-form 4x4 least squares solve, instead of a nine-dimensional
// nonconvex optimization that is famous for finding a different answer every
// time it is run.
//
// A fit is only counted as a bubble if it passes the standard qualification
// filters from the LPPLS literature:
//   * 0.1 <= m <= 0.9        - a genuine singularity, not a line or a spike
//   * 6 <= omega <= 13       - the empirically observed log-periodic band
//   * B < 0                  - price accelerating UPWARD (positive bubble)
//   * damping = m|B| / (omega|C|) >= 0.8   (Bothmer & Meister's condition)
//     which keeps the oscillation from dominating the trend, i.e. rejects
//     fits where the "bubble" is really a wobble.
// Confidence is the fraction of window lengths whose best fit qualifies -
// the multi-scale agreement that Sornette's group uses in place of the
// significance of any single fit.
//
// The trade: hold the trend while no bubble is detected, and stand aside
// while one is. That is the honest use of the signal. It predicts elevated
// crash HAZARD, not a date, and it says nothing about the downside - so
// mapping it to "go flat" rather than "go short" is not timidity, it is the
// most the model actually claims.
// ---------------------------------------------------------------------------
struct LpplsParams {
    int window = 250;            // longest fitting window, in bars
    int refitEvery = 10;         // bars between refits (fitting is the cost)
    double confThreshold = 0.5;  // fraction of scales that must qualify
    int trendWindow = 100;       // the trend that is held when no bubble is seen
};

class LpplsBubbleStrategy : public Strategy {
public:
    explicit LpplsBubbleStrategy(LpplsParams p = {}) : p_(p) {}
    std::string name() const override { return "lppls_bubble"; }

    void prepare(const CandleSeries& s) override {
        const auto logC = zoo::logOf(s.close);
        size_t n = logC.size();
        conf_.assign(n, 0.0);
        trend_ = indicators::sma(s.close, std::max(5, p_.trendWindow));

        int wMax = std::max(80, p_.window);
        // Three scales, geometrically spaced. Multi-scale agreement is the
        // whole basis of the confidence measure, so one window is not an
        // option; more than three is not affordable inside a search.
        const double scaleFrac[3] = {1.0, 0.7, 0.5};
        const double mGrid[3] = {0.3, 0.5, 0.7};
        const double omGrid[3] = {6.5, 9.0, 12.0};
        const double tcFrac[3] = {0.03, 0.10, 0.25};

        // The design matrix is CONSTANT across refits.
        //
        // t_c is parameterized relative to the end of the fitting window, so
        // for a fixed (window length, m, omega, t_c offset) the three
        // nonlinear basis vectors - and therefore X'X and its inverse - are
        // identical no matter where in the series the window sits. Only X'y
        // changes. Precomputing them turns each refit from 81 least-squares
        // problems with O(w) transcendental evaluations each into 81 sets of
        // three dot products, which is what makes this family affordable
        // inside a search rather than a curiosity that runs once.
        bases_.clear();
        for (int si = 0; si < 3; ++si) {
            int w = static_cast<int>(wMax * scaleFrac[si]);
            if (w < 60) continue;
            for (double m : mGrid)
                for (double om : omGrid)
                    for (double tf : tcFrac)
                        addBasis(si, w, m, om, static_cast<double>(w - 1) + tf * w);
        }
        if (bases_.empty()) return;

        // Prefix sums of y and y^2 give the constant term's X'y entry and the
        // window's y'y in O(1), so SSE = y'y - beta'X'y costs nothing.
        std::vector<double> ps(n + 1, 0.0), ps2(n + 1, 0.0);
        bool anyBad = false;
        for (size_t i = 0; i < n; ++i) {
            double v = zoo::ok(logC[i]) ? logC[i] : 0.0;
            if (!zoo::ok(logC[i])) anyBad = true;
            ps[i + 1] = ps[i] + v;
            ps2[i + 1] = ps2[i] + v * v;
        }
        (void)anyBad;

        int step = std::max(1, p_.refitEvery);
        double held = 0.0;
        int nScales = 0;
        for (int si = 0; si < 3; ++si)
            if (static_cast<int>(wMax * scaleFrac[si]) >= 60) ++nScales;

        for (size_t i = 0; i < n; ++i) {
            if (i >= static_cast<size_t>(wMax) && (i - wMax) % step == 0) {
                bool scaleQualified[3] = {false, false, false};
                bool scaleUsable[3] = {false, false, false};
                double bestSse[3] = {1e300, 1e300, 1e300};
                double bestM[3]{}, bestOm[3]{}, bestB[3]{}, bestC[3]{};
                for (const auto& b : bases_) {
                    if (i + 1 < static_cast<size_t>(b.w)) continue;
                    size_t start = i + 1 - static_cast<size_t>(b.w);
                    if (!windowClean(logC, start, b.w)) continue;
                    scaleUsable[b.scale] = true;
                    double xty[4];
                    xty[0] = ps[i + 1] - ps[start];
                    xty[1] = dot(b.b1, logC, start);
                    xty[2] = dot(b.b2, logC, start);
                    xty[3] = dot(b.b3, logC, start);
                    double coef[4];
                    for (int r = 0; r < 4; ++r) {
                        double acc = 0.0;
                        for (int c = 0; c < 4; ++c) acc += b.inv[r][c] * xty[c];
                        coef[r] = acc;
                    }
                    double yy = ps2[i + 1] - ps2[start];
                    double sse = yy - (coef[0] * xty[0] + coef[1] * xty[1] +
                                        coef[2] * xty[2] + coef[3] * xty[3]);
                    if (!std::isfinite(sse) || sse >= bestSse[b.scale]) continue;
                    bestSse[b.scale] = sse;
                    bestM[b.scale] = b.m;
                    bestOm[b.scale] = b.om;
                    bestB[b.scale] = coef[1];
                    bestC[b.scale] = std::sqrt(coef[2] * coef[2] + coef[3] * coef[3]);
                }
                int tried = 0, qualified = 0;
                for (int si = 0; si < 3; ++si) {
                    if (!scaleUsable[si] || bestSse[si] > 1e299) continue;
                    ++tried;
                    scaleQualified[si] = qualifies(bestM[si], bestOm[si], bestB[si], bestC[si]);
                    if (scaleQualified[si]) ++qualified;
                }
                held = tried > 0 ? static_cast<double>(qualified) / tried : 0.0;
            }
            conf_[i] = held;
        }
        (void)nScales;
    }

    Signal onBar(const CandleSeries& s, size_t i, const PositionContext&) override {
        if (i >= conf_.size() || !zoo::ok(trend_[i])) return Signal::Hold;
        if (conf_[i] >= p_.confThreshold) return Signal::Sell;   // bubble: stand aside
        return s.close[i] > trend_[i] ? Signal::Buy : Signal::Sell;
    }

private:
    struct Basis {
        int scale = 0;
        int w = 0;
        double m = 0, om = 0, tc = 0;
        std::vector<double> b1, b2, b3;   // b0 is the all-ones column
        double inv[4][4]{};               // (X'X)^-1
    };

    static bool windowClean(const std::vector<double>& y, size_t start, int w) {
        for (int t = 0; t < w; ++t) if (!zoo::ok(y[start + t])) return false;
        return true;
    }

    static double dot(const std::vector<double>& b, const std::vector<double>& y, size_t start) {
        double acc = 0.0;
        for (size_t t = 0; t < b.size(); ++t) acc += b[t] * y[start + t];
        return acc;
    }

    // The qualification filters from the LPPLS literature. A fit that does
    // not pass these is a curve through some points, not a bubble.
    static bool qualifies(double m, double om, double B, double C) {
        if (m < 0.1 || m > 0.9) return false;
        if (om < 6.0 || om > 13.0) return false;
        if (B >= 0.0) return false;                    // not a POSITIVE bubble
        if (C <= 0.0) return true;                     // no oscillation to damp
        return m * std::fabs(B) / (om * C) >= 0.8;     // Bothmer-Meister damping
    }

    void addBasis(int scale, int w, double m, double om, double tc) {
        Basis b;
        b.scale = scale; b.w = w; b.m = m; b.om = om; b.tc = tc;
        b.b1.resize(w); b.b2.resize(w); b.b3.resize(w);
        double xtx[4][4] = {{0}};
        for (int t = 0; t < w; ++t) {
            double tau = tc - t;
            if (tau <= 1e-9) return;
            double pw = std::pow(tau, m);
            double lt = std::log(tau);
            b.b1[t] = pw;
            b.b2[t] = pw * std::cos(om * lt);
            b.b3[t] = pw * std::sin(om * lt);
            const double col[4] = {1.0, b.b1[t], b.b2[t], b.b3[t]};
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) xtx[r][c] += col[r] * col[c];
        }
        if (!invert4(xtx, b.inv)) return;   // degenerate design: drop this combo
        bases_.push_back(std::move(b));
    }

    // Gauss-Jordan with partial pivoting on a 4x4. Small and dense enough
    // that nothing more elaborate is warranted, and a failed inversion just
    // removes one grid point rather than failing the whole fit.
    static bool invert4(const double a[4][4], double out[4][4]) {
        double m[4][8];
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) { m[r][c] = a[r][c]; m[r][c + 4] = (r == c) ? 1.0 : 0.0; }
        }
        for (int col = 0; col < 4; ++col) {
            int piv = col;
            for (int r = col + 1; r < 4; ++r)
                if (std::fabs(m[r][col]) > std::fabs(m[piv][col])) piv = r;
            if (std::fabs(m[piv][col]) < 1e-14) return false;
            if (piv != col) for (int c = 0; c < 8; ++c) std::swap(m[col][c], m[piv][c]);
            double d = m[col][col];
            for (int c = 0; c < 8; ++c) m[col][c] /= d;
            for (int r = 0; r < 4; ++r) {
                if (r == col) continue;
                double f = m[r][col];
                if (f == 0.0) continue;
                for (int c = 0; c < 8; ++c) m[r][c] -= f * m[col][c];
            }
        }
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                out[r][c] = m[r][c + 4];
                if (!std::isfinite(out[r][c])) return false;
            }
        return true;
    }

    LpplsParams p_;
    std::vector<Basis> bases_;
    std::vector<double> conf_, trend_;
};

} // namespace trader
