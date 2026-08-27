#include "pencil.h"

#include <algorithm>
#include <cmath>
#include <limits>

// Everything below is deliberately self-contained dense linear algebra for
// SMALL matrices (the Hankel block is at most a few hundred rows and the
// eigenproblem is at most a few dozen square). Nothing here is asymptotically
// clever; it is chosen for numerical behaviour, because the whole point of
// this estimator is that the interesting modes sit 8-10 orders of magnitude
// below the leading one and naive formulations quietly delete them.
namespace trader::mathx {
namespace {

using Cd = std::complex<double>;

constexpr double kEps = std::numeric_limits<double>::epsilon();

struct CMat {
    int rows = 0;
    int cols = 0;
    std::vector<Cd> a;

    CMat() = default;
    CMat(int r, int c)
        : rows(r), cols(c), a(static_cast<size_t>(r) * static_cast<size_t>(c), Cd(0.0, 0.0)) {}

    Cd& operator()(int i, int j) { return a[static_cast<size_t>(i) * cols + j]; }
    const Cd& operator()(int i, int j) const { return a[static_cast<size_t>(i) * cols + j]; }
};

bool finiteC(const Cd& v) { return std::isfinite(v.real()) && std::isfinite(v.imag()); }

bool allFinite(const CMat& m) {
    for (const Cd& v : m.a) {
        if (!finiteC(v)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// One-sided (Hestenes) Jacobi SVD: returns the singular values and the RIGHT
// singular vectors of A, sorted by descending singular value, with
//     A = U * diag(sv) * V^H.
//
// The rotations are computed from inner products of the CURRENT columns and
// applied to A itself, so the Gram matrix is never materialised. That is the
// whole reason to prefer this over "eigen-decompose A^H A with two-sided
// Jacobi": forming A^H A in floating point destroys every singular value
// below sqrt(eps)*sv[0], and this estimator routinely needs vectors belonging
// to singular values ~1e-9 of the leading one.
// ---------------------------------------------------------------------------
bool jacobiRightSingularVectors(CMat A, std::vector<double>& sv, CMat& V) {
    const int m = A.rows;
    const int n = A.cols;
    if (m < 1 || n < 1) return false;

    V = CMat(n, n);
    for (int i = 0; i < n; ++i) V(i, i) = Cd(1.0, 0.0);

    // Largest column norm of the ORIGINAL matrix; both stopping tests are
    // measured against it and it never changes as the columns rotate.
    double scaleSq = 0.0;
    for (int j = 0; j < n; ++j) {
        double t = 0.0;
        for (int i = 0; i < m; ++i) t += std::norm(A(i, j));
        scaleSq = std::max(scaleSq, t);
    }
    if (!(scaleSq > 0.0) || !std::isfinite(scaleSq)) return false;

    // A column that has shrunk to the rounding level of the matrix spans the
    // numerical null space: its direction is pure noise, so orthogonalising it
    // against a full-size column does not improve anything - it just refills
    // it with fresh O(eps*||A||) noise, and the sweep then never terminates.
    // Freeze those columns instead.
    const double nullSq = scaleSq * (static_cast<double>(m) * kEps) *
                          (static_cast<double>(m) * kEps);
    // Orthogonality tolerance. The textbook eps*sqrt(alpha*beta) is a factor
    // ~m below the rounding error of the dot product itself, so a strict
    // cyclic sweep can chase its own noise forever; m*eps is the honest bound.
    const double orthTol = static_cast<double>(m) * kEps;

    bool converged = false;
    for (int sweep = 0; sweep < 60 && !converged; ++sweep) {
        converged = true;
        for (int p = 0; p < n - 1; ++p) {
            for (int q = p + 1; q < n; ++q) {
                double alpha = 0.0, beta = 0.0;
                Cd gamma(0.0, 0.0);
                for (int i = 0; i < m; ++i) {
                    alpha += std::norm(A(i, p));
                    beta += std::norm(A(i, q));
                    gamma += std::conj(A(i, p)) * A(i, q);
                }
                const double g = std::abs(gamma);
                if (g == 0.0) continue;
                if (alpha <= nullSq || beta <= nullSq) continue;
                // Already orthogonal to working precision - rotating would
                // only inject rounding noise.
                if (g <= orthTol * std::sqrt(alpha * beta)) continue;
                converged = false;

                // Rotate the phase of column q so the inner product is real
                // and positive, then apply the ordinary real Jacobi rotation.
                const Cd ph = std::conj(gamma) / g;
                const double zeta = (beta - alpha) / (2.0 * g);
                const double t = (zeta >= 0.0 ? 1.0 : -1.0) /
                                 (std::abs(zeta) + std::sqrt(1.0 + zeta * zeta));
                const double c = 1.0 / std::sqrt(1.0 + t * t);
                const double s = c * t;

                for (int i = 0; i < m; ++i) {
                    const Cd ap = A(i, p);
                    const Cd aq = ph * A(i, q);
                    A(i, p) = c * ap - s * aq;
                    A(i, q) = s * ap + c * aq;
                }
                for (int i = 0; i < n; ++i) {
                    const Cd vp = V(i, p);
                    const Cd vq = ph * V(i, q);
                    V(i, p) = c * vp - s * vq;
                    V(i, q) = s * vp + c * vq;
                }
            }
        }
    }
    // Non-convergence after 60 sweeps is not treated as failure: the sweep is
    // monotone, so the subspace is still the best available and the downstream
    // rank/least-squares guards will reject it if it is genuinely unusable.
    (void)converged;

    // Column norms of the rotated A are the singular values.
    sv.assign(n, 0.0);
    for (int j = 0; j < n; ++j) {
        double t = 0.0;
        for (int i = 0; i < m; ++i) t += std::norm(A(i, j));
        sv[j] = std::sqrt(t);
        if (!std::isfinite(sv[j])) return false;
    }

    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&sv](int x, int y) { return sv[x] > sv[y]; });

    std::vector<double> svSorted(n);
    CMat Vs(n, n);
    for (int j = 0; j < n; ++j) {
        svSorted[j] = sv[order[j]];
        for (int i = 0; i < n; ++i) Vs(i, j) = V(i, order[j]);
    }
    sv.swap(svSorted);
    V = std::move(Vs);
    return true;
}

// ---------------------------------------------------------------------------
// Least squares min ||A X - B|| for a tall-thin A, by Gram-Schmidt with one
// reorthogonalisation pass ("twice is enough"). Reorthogonalised CGS is as
// accurate as Householder QR at these sizes and, unlike the normal equations
// A^H A X = A^H B, does not square cond(A).
// ---------------------------------------------------------------------------
bool leastSquares(const CMat& A, const CMat& B, CMat& X) {
    const int m = A.rows;
    const int n = A.cols;
    const int k = B.cols;
    if (n < 1 || m < n || B.rows != m || k < 1) return false;

    CMat Q = A;
    CMat R(n, n);
    for (int j = 0; j < n; ++j) {
        double initial = 0.0;
        for (int i = 0; i < m; ++i) initial += std::norm(Q(i, j));
        initial = std::sqrt(initial);
        if (!(initial > 0.0) || !std::isfinite(initial)) return false;

        for (int pass = 0; pass < 2; ++pass) {
            for (int c = 0; c < j; ++c) {
                Cd t(0.0, 0.0);
                for (int i = 0; i < m; ++i) t += std::conj(Q(i, c)) * Q(i, j);
                for (int i = 0; i < m; ++i) Q(i, j) -= t * Q(i, c);
                R(c, j) += t;
            }
        }
        double nrm = 0.0;
        for (int i = 0; i < m; ++i) nrm += std::norm(Q(i, j));
        nrm = std::sqrt(nrm);
        // A column that collapses under orthogonalisation means A is rank
        // deficient at the requested model order; caller must back off.
        if (!std::isfinite(nrm) || nrm <= 1e-14 * initial) return false;
        R(j, j) = Cd(nrm, 0.0);
        for (int i = 0; i < m; ++i) Q(i, j) /= nrm;
    }

    X = CMat(n, k);
    std::vector<Cd> rhs(n);
    for (int c = 0; c < k; ++c) {
        for (int i = 0; i < n; ++i) {
            Cd t(0.0, 0.0);
            for (int r = 0; r < m; ++r) t += std::conj(Q(r, i)) * B(r, c);
            rhs[i] = t;
        }
        for (int i = n - 1; i >= 0; --i) {
            Cd t = rhs[i];
            for (int j = i + 1; j < n; ++j) t -= R(i, j) * X(j, c);
            X(i, c) = t / R(i, i);
        }
    }
    return allFinite(X);
}

// ---------------------------------------------------------------------------
// Eigenvalues of a small dense complex matrix: unitary Householder reduction
// to upper Hessenberg form, then a Wilkinson-shifted complex QR iteration
// driven by Givens rotations. Complex arithmetic means single shifts suffice -
// no Francis double-shift bookkeeping is needed to get conjugate pairs out of
// a real matrix.
// ---------------------------------------------------------------------------
void toHessenberg(CMat& H) {
    const int n = H.rows;
    std::vector<Cd> v(n, Cd(0.0, 0.0));
    for (int k = 0; k + 2 < n; ++k) {
        double tail = 0.0;
        for (int i = k + 2; i < n; ++i) tail += std::norm(H(i, k));
        if (tail == 0.0) continue;  // already in Hessenberg form at this column

        double xnorm = tail + std::norm(H(k + 1, k));
        xnorm = std::sqrt(xnorm);
        const Cd x0 = H(k + 1, k);
        const double a0 = std::abs(x0);
        // Choosing alpha anti-parallel to x0 maximises ||v||, i.e. avoids the
        // cancellation that makes the naive v = x - ||x|| e1 unstable.
        const Cd phase = (a0 > 0.0) ? x0 / a0 : Cd(1.0, 0.0);
        const Cd alpha = -phase * xnorm;

        for (int i = k + 1; i < n; ++i) v[i] = H(i, k);
        v[k + 1] -= alpha;
        double vnorm = 0.0;
        for (int i = k + 1; i < n; ++i) vnorm += std::norm(v[i]);
        vnorm = std::sqrt(vnorm);
        if (!(vnorm > 0.0)) continue;
        for (int i = k + 1; i < n; ++i) v[i] /= vnorm;

        for (int j = 0; j < n; ++j) {  // H <- (I - 2 v v^H) H
            Cd s(0.0, 0.0);
            for (int i = k + 1; i < n; ++i) s += std::conj(v[i]) * H(i, j);
            s *= 2.0;
            for (int i = k + 1; i < n; ++i) H(i, j) -= s * v[i];
        }
        for (int i = 0; i < n; ++i) {  // H <- H (I - 2 v v^H)
            Cd s(0.0, 0.0);
            for (int j = k + 1; j < n; ++j) s += H(i, j) * v[j];
            s *= 2.0;
            for (int j = k + 1; j < n; ++j) H(i, j) -= s * std::conj(v[j]);
        }
        for (int i = k + 2; i < n; ++i) H(i, k) = Cd(0.0, 0.0);
    }
}

void eig2x2(const Cd& a, const Cd& b, const Cd& c, const Cd& d, Cd& e1, Cd& e2) {
    const Cd det = a * d - b * c;
    const Cd half = 0.5 * (a + d);
    const Cd delta = 0.5 * (a - d);
    const Cd disc = std::sqrt(delta * delta + b * c);
    const Cd r1 = half + disc;
    const Cd r2 = half - disc;
    // Take the well-conditioned root from the sum and the other from the
    // product, so the small root is not the difference of two near-equals.
    if (std::abs(r1) >= std::abs(r2)) {
        e1 = r1;
        e2 = (std::abs(r1) > 0.0) ? det / r1 : r2;
    } else {
        e1 = r2;
        e2 = (std::abs(r2) > 0.0) ? det / r2 : r1;
    }
}

Cd wilkinsonShift(const CMat& H, int hi) {
    const Cd a = H(hi - 1, hi - 1);
    const Cd b = H(hi - 1, hi);
    const Cd c = H(hi, hi - 1);
    const Cd d = H(hi, hi);
    const Cd bc = b * c;
    const Cd delta = 0.5 * (a - d);
    if (bc == Cd(0.0, 0.0)) return d;
    const Cd disc = std::sqrt(delta * delta + bc);
    const Cd p1 = delta + disc;
    const Cd p2 = delta - disc;
    const Cd den = (std::abs(p1) >= std::abs(p2)) ? p1 : p2;
    if (std::abs(den) == 0.0) return d;
    return d - bc / den;  // the 2x2 eigenvalue closest to H(hi,hi)
}

void qrStep(CMat& H, int lo, int hi, const Cd& mu) {
    const int span = hi - lo;
    std::vector<Cd> cs(span), sn(span);

    for (int i = lo; i <= hi; ++i) H(i, i) -= mu;

    for (int k = lo; k < hi; ++k) {  // left: annihilate the subdiagonal
        const Cd a = H(k, k);
        const Cd b = H(k + 1, k);
        const double r = std::sqrt(std::norm(a) + std::norm(b));
        Cd c(1.0, 0.0), s(0.0, 0.0);
        if (r > 0.0) {
            c = a / r;
            s = b / r;
        }
        cs[k - lo] = c;
        sn[k - lo] = s;
        for (int j = k; j <= hi; ++j) {
            const Cd h0 = H(k, j);
            const Cd h1 = H(k + 1, j);
            H(k, j) = std::conj(c) * h0 + std::conj(s) * h1;
            H(k + 1, j) = -s * h0 + c * h1;
        }
    }
    for (int k = lo; k < hi; ++k) {  // right: RQ, restoring Hessenberg form
        const Cd c = cs[k - lo];
        const Cd s = sn[k - lo];
        const int last = std::min(k + 2, hi);
        for (int i = lo; i <= last; ++i) {
            const Cd h0 = H(i, k);
            const Cd h1 = H(i, k + 1);
            H(i, k) = c * h0 + s * h1;
            H(i, k + 1) = -std::conj(s) * h0 + std::conj(c) * h1;
        }
    }

    for (int i = lo; i <= hi; ++i) H(i, i) += mu;
}

bool hessenbergQr(CMat& H, std::vector<Cd>& eig) {
    const int n = H.rows;
    eig.assign(n, Cd(0.0, 0.0));

    double anorm = 0.0;
    for (int i = 0; i < n; ++i) {
        for (int j = std::max(i - 1, 0); j < n; ++j) anorm += std::abs(H(i, j));
    }
    if (!(anorm > 0.0)) return true;  // nilpotent/zero: all eigenvalues are 0

    int hi = n - 1;
    int its = 0;
    while (hi >= 0) {
        int lo = hi;
        while (lo > 0) {
            double scale = std::abs(H(lo - 1, lo - 1)) + std::abs(H(lo, lo));
            if (scale == 0.0) scale = anorm;
            if (std::abs(H(lo, lo - 1)) <= kEps * scale) {
                H(lo, lo - 1) = Cd(0.0, 0.0);
                break;
            }
            --lo;
        }

        if (lo == hi) {
            eig[hi] = H(hi, hi);
            --hi;
            its = 0;
            continue;
        }
        if (lo == hi - 1) {
            eig2x2(H(hi - 1, hi - 1), H(hi - 1, hi), H(hi, hi - 1), H(hi, hi),
                   eig[hi - 1], eig[hi]);
            hi -= 2;
            its = 0;
            continue;
        }

        if (++its > 100) return false;
        // Every so often take a deliberately wrong shift; a few pathological
        // matrices cycle forever on the Wilkinson shift alone.
        const Cd mu = (its % 17 == 0)
                          ? H(hi, hi) + Cd(std::abs(H(hi, hi - 1)), 0.0)
                          : wilkinsonShift(H, hi);
        if (!finiteC(mu)) return false;
        qrStep(H, lo, hi, mu);
        if (!allFinite(H)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Fallback eigensolver: Faddeev-LeVerrier characteristic polynomial followed
// by Durand-Kerner (Weierstrass) root finding. Reached only if the QR
// iteration above fails to converge. It is kept deliberately last because
// Faddeev-LeVerrier accumulates catastrophic cancellation - it is fine to
// M ~ 8 and meaningless well before the M = 23 the Cantor calibration needs -
// so a result from here should be treated as a rescue, not as a check.
// ---------------------------------------------------------------------------
bool eigenvaluesCharPoly(const CMat& A, std::vector<Cd>& eig) {
    const int n = A.rows;
    if (n < 1) return false;
    std::vector<Cd> coef(n + 1, Cd(0.0, 0.0));  // monic: coef[0]*x^n + ...
    coef[0] = Cd(1.0, 0.0);

    CMat M = A;
    for (int k = 1; k <= n; ++k) {
        Cd tr(0.0, 0.0);
        for (int i = 0; i < n; ++i) tr += M(i, i);
        coef[k] = -tr / static_cast<double>(k);
        if (!finiteC(coef[k])) return false;
        if (k == n) break;
        for (int i = 0; i < n; ++i) M(i, i) += coef[k];
        CMat P(n, n);
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                Cd s(0.0, 0.0);
                for (int t = 0; t < n; ++t) s += A(i, t) * M(t, j);
                P(i, j) = s;
            }
        }
        M = std::move(P);
    }

    // Durand-Kerner from the classic spiral seed (0.4 + 0.9i)^k, which avoids
    // the symmetric traps that a real or equispaced seed falls into.
    std::vector<Cd> root(n);
    Cd seed(0.4, 0.9);
    Cd p(1.0, 0.0);
    for (int i = 0; i < n; ++i) {
        root[i] = p;
        p *= seed;
    }
    for (int iter = 0; iter < 1000; ++iter) {
        double shift = 0.0;
        for (int i = 0; i < n; ++i) {
            Cd num = coef[0];
            for (int k = 1; k <= n; ++k) num = num * root[i] + coef[k];
            Cd den(1.0, 0.0);
            for (int j = 0; j < n; ++j) {
                if (j != i) den *= (root[i] - root[j]);
            }
            if (std::abs(den) == 0.0) continue;
            const Cd d = num / den;
            if (!finiteC(d)) return false;
            root[i] -= d;
            shift = std::max(shift, std::abs(d));
        }
        if (shift <= 1e-14 * (1.0 + std::abs(root[0]))) break;
    }
    for (const Cd& r : root) {
        if (!finiteC(r)) return false;
    }
    eig = root;
    return true;
}

bool eigenvalues(const CMat& A, std::vector<Cd>& eig) {
    CMat H = A;
    toHessenberg(H);
    if (allFinite(H) && hessenbergQr(H, eig)) return true;
    return eigenvaluesCharPoly(A, eig);
}

// ---------------------------------------------------------------------------
// Automatic model order from the singular-value profile.
//
// The naive "keep every singular value above eps*sv[0]" rule that reference
// implementations use is inert on real data: at 20 dB SNR every singular value
// clears a 1e-8 relative floor, so the rule retains the full rank and the
// estimator returns a forest of noise modes. What actually separates signal
// from noise is the ELBOW - a sharp relative drop followed by the flat noise
// plateau - so that is the criterion here, with energyThreshold demoted to a
// floor that bounds the search and maxModelOrder capping it.
//
// Note it takes the LAST significant drop, not the largest one. Signal
// singular values often step down more than once (a weak third mode sits well
// below the dominant pair but still well above the noise), and picking the
// single largest gap then truncates the model at the first step: on a 40 dB
// three-mode test that costs a factor ~30 in the recovered exponent.
// ---------------------------------------------------------------------------
int chooseModelOrder(const std::vector<double>& sv, int r, double energyThreshold,
                     int maxModelOrder) {
    const int mmax = std::max(1, r - 1);
    const double floorValue = (energyThreshold > 0.0 ? energyThreshold : 0.0) * sv[0];

    int cap = 0;
    for (int k = 0; k < r; ++k) {
        if (sv[k] > floorValue) ++cap;
        else break;
    }
    cap = std::max(1, std::min(cap, mmax));
    if (maxModelOrder > 0) cap = std::max(1, std::min(cap, maxModelOrder));

    const double kInf = std::numeric_limits<double>::infinity();
    std::vector<double> gap;
    gap.reserve(static_cast<size_t>(cap));
    for (int M = 1; M <= cap && M < r; ++M) {
        if (!(sv[M - 1] > 0.0)) break;  // past the numerical rank: gaps are 0/0
        gap.push_back(sv[M] > 0.0 ? sv[M - 1] / sv[M] : kInf);
    }
    if (gap.empty()) return std::max(1, std::min(1, cap));

    double bestGap = 0.0;
    int argMax = 1;
    for (size_t i = 0; i < gap.size(); ++i) {
        if (gap[i] > bestGap) {
            bestGap = gap[i];
            argMax = static_cast<int>(i) + 1;
        }
    }
    // "Significant" = at least a real factor-1.5 step AND at least half the
    // deepest step seen. A spectrum with no such step (pure noise) falls back
    // to the deepest step, which is the most defensible guess available.
    const double thresh = std::max(1.5, 0.5 * bestGap);
    int chosen = argMax;
    for (size_t i = 0; i < gap.size(); ++i) {
        if (gap[i] >= thresh) chosen = static_cast<int>(i) + 1;
    }
    return std::max(1, std::min(chosen, cap));
}

} // namespace

double PencilMode::magnitude() const { return std::abs(amplitude); }

std::vector<PencilMode> matrixPencil(const std::vector<Cd>& y, double delta,
                                     const PencilOptions& opts) {
    std::vector<PencilMode> out;
    const int N = static_cast<int>(y.size());
    if (N < 4) return out;
    if (!std::isfinite(delta) || delta == 0.0) return out;
    for (const Cd& v : y) {
        if (!finiteC(v)) return out;
    }

    int L = (opts.pencilParam < 0) ? N / 2 : opts.pencilParam;
    L = std::max(2, std::min(L, N - 2));
    const int rows = N - L;
    const int cols = L + 1;
    if (rows < 2 || cols < 3) return out;

    CMat Y(rows, cols);
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) Y(i, j) = y[i + j];
    }

    std::vector<double> sv;
    CMat V;
    if (!jacobiRightSingularVectors(Y, sv, V)) return out;
    const int r = std::min(rows, cols);
    if (r < 2 || !(sv[0] > 0.0)) return out;

    int M = opts.modelOrder;
    if (M <= 0) M = chooseModelOrder(sv, r, opts.energyThreshold, opts.maxModelOrder);
    M = std::max(1, std::min(M, std::max(1, r - 1)));

    // The signal subspace we need is the ROW space of Y: rows of Y are
    // Vandermonde vectors (1, z, ..., z^L) in the nodes themselves. With
    // Y = U S V^H those rows live in span{conj(V_k)}, NOT span{V_k}. Taking
    // V unconjugated here is a real (if well-camouflaged) bug: it returns
    // conj(z) instead of z, which for real input is invisible - the node set
    // is conjugate-closed, so it merely swaps a pair - but for genuinely
    // complex data it returns reflected nodes and, worse, amplitudes fitted
    // against those wrong nodes.
    CMat W1(cols - 1, M), W2(cols - 1, M);
    for (int i = 0; i < cols - 1; ++i) {
        for (int j = 0; j < M; ++j) {
            W1(i, j) = std::conj(V(i, j));
            W2(i, j) = std::conj(V(i + 1, j));
        }
    }

    CMat A;
    if (!leastSquares(W1, W2, A)) return out;  // A = pinv(W1) * W2

    std::vector<Cd> z;
    if (!eigenvalues(A, z)) return out;
    if (static_cast<int>(z.size()) != M) return out;

    // Amplitudes: min ||Vand c - y|| over the recovered nodes.
    CMat Vand(N, M);
    for (int j = 0; j < M; ++j) {
        Cd p(1.0, 0.0);
        for (int i = 0; i < N; ++i) {
            Vand(i, j) = p;
            p *= z[j];
        }
    }
    if (!allFinite(Vand)) return out;  // |z|^N overflowed; the fit is meaningless
    CMat B(N, 1);
    for (int i = 0; i < N; ++i) B(i, 0) = y[i];
    CMat C;
    if (!leastSquares(Vand, B, C)) return out;

    out.reserve(static_cast<size_t>(M));
    for (int j = 0; j < M; ++j) {
        const double mag = std::abs(z[j]);
        if (!(mag > 0.0)) continue;  // z = 0 has no finite exponent
        PencilMode mode;
        // Re(omega) from |z| is single-valued; Im(omega) from arg(z) is only
        // defined mod 2*pi/delta (the caller must reject aliased modes).
        mode.omega = Cd(std::log(mag) / delta, std::arg(z[j]) / delta);
        mode.amplitude = C(j, 0);
        if (!finiteC(mode.omega) || !finiteC(mode.amplitude)) continue;
        out.push_back(mode);
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const PencilMode& a, const PencilMode& b) {
                         return a.magnitude() > b.magnitude();
                     });
    return out;
}

std::vector<PencilMode> matrixPencil(const std::vector<double>& y, double delta,
                                     const PencilOptions& opts) {
    std::vector<Cd> yc;
    yc.reserve(y.size());
    for (double v : y) yc.emplace_back(v, 0.0);
    return matrixPencil(yc, delta, opts);
}

std::vector<double> decayRates(const std::vector<double>& acf, int maxRates,
                               int modelOrder) {
    std::vector<double> rates;
    if (maxRates <= 0) return rates;

    PencilOptions opts;
    opts.modelOrder = modelOrder;
    const std::vector<PencilMode> modes = matrixPencil(acf, 1.0, opts);

    for (const PencilMode& m : modes) {
        // A real eigenvalue of the real pencil comes back with Im(omega)
        // exactly zero; anything else is an oscillatory (or sign-alternating,
        // arg(z) = pi) mode and is not a decay timescale.
        if (std::abs(m.omega.imag()) > 1e-6) continue;
        const double rate = -m.omega.real();
        if (!(rate > 0.0) || !std::isfinite(rate)) continue;  // flat or growing
        rates.push_back(rate);
    }

    std::sort(rates.begin(), rates.end());  // slowest decay first
    if (static_cast<int>(rates.size()) > maxRates) {
        rates.resize(static_cast<size_t>(maxRates));
    }
    return rates;
}

} // namespace trader::mathx
