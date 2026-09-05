#pragma once
#include "../strategy.h"
#include "zoo_common.h"
#include <cmath>
#include <string>
#include <vector>

namespace trader {

// Short-horizon mean reversion: buy a sharp, statistically extreme drop and
// hold for a fixed number of bars.
//
// This is the first strategy in the zoo that is NOT trend following. It exists
// because addendum 12 killed intraday TREND on arithmetic - a 15m trend signal
// had real gross edge (Sharpe 1.24) and lost 99% of it to turnover, because
// trend continuation over an hour is worth a few basis points and a round trip
// costs ~30 (0.125% taker each side + slippage; the 2026-08-27 /feeinfo check
// showed spot maker is 0.115%, so there is no maker escape). Mean reversion
// after a violent move is the one intraday effect whose per-trade magnitude is
// measured in tens of basis points rather than single digits, so it is the only
// candidate that can clear the wall.
//
// Measured on 15m dev bars across the 8 live coins, mean forward return over
// the 2 hours after a drop beyond k sigma:
//     k=2: 23bps   k=3: 35bps   k=4: 58bps    (unconditional baseline: 5bps)
// Monotone in k, which is what a real mechanism looks like - deeper forced
// selling, bigger bounce - rather than a fitted threshold.
//
// The exit is a pure time stop. That is deliberate: a price-based exit (target
// or stop) is exactly the fixed bracket that destroyed a real signal in
// addendum 11, and every extra price parameter is another dimension to overfit.
// Holding a fixed number of bars costs nothing to specify and cannot be tuned
// into a mirage as easily.
struct DipReversionParams {
    int    lookback = 4;        // bars over which the drop is measured (1h at 15m)
    int    volWindow = 96;      // bars for the sigma estimate (1 day at 15m)
    double entrySigmas = 4.0;   // how extreme the drop must be
    int    holdBars = 8;        // time exit (2h at 15m)
};

class DipReversionStrategy : public Strategy {
public:
    explicit DipReversionStrategy(DipReversionParams p = {}) : p_(p) {}
    std::string name() const override { return "dip_reversion"; }

    void prepare(const CandleSeries& s) override {
        const size_t n = s.size();
        z_.assign(n, 0.0);
        if (n == 0) return;
        const int L = std::max(1, p_.lookback);
        const int W = std::max(2, p_.volWindow);

        // r[i] = return over the trailing L bars, decided on close[i].
        std::vector<double> r(n, 0.0);
        std::vector<char> ok(n, 0);
        for (size_t i = static_cast<size_t>(L); i < n; ++i) {
            double base = s.close[i - L];
            if (base > 0.0) { r[i] = s.close[i] / base - 1.0; ok[i] = 1; }
        }
        // Rolling mean/variance of r over the previous W valid bars. Running
        // sums rather than a nested loop: at 15m a store is ~400k bars and the
        // quadratic version made the sweep unusable.
        double sum = 0.0, sumsq = 0.0;
        int count = 0;
        for (size_t i = static_cast<size_t>(L); i < n; ++i) {
            if (count >= W) {
                size_t drop = i - static_cast<size_t>(W);
                if (ok[drop]) { sum -= r[drop]; sumsq -= r[drop] * r[drop]; --count; }
            }
            if (count >= 2) {
                double mean = sum / count;
                double var = sumsq / count - mean * mean;
                double sd = var > 0.0 ? std::sqrt(var) : 0.0;
                if (sd > 0.0 && ok[i]) z_[i] = (r[i] - mean) / sd;
            }
            if (ok[i]) { sum += r[i]; sumsq += r[i] * r[i]; ++count; }
        }
    }

    Signal onBar(const CandleSeries&, size_t i, const PositionContext& pos) override {
        if (i >= z_.size()) return Signal::Hold;
        if (!pos.inPosition) return z_[i] < -p_.entrySigmas ? Signal::Buy : Signal::Hold;
        // Time exit only. entryIndex is the bar the position was opened on.
        if (i >= pos.entryIndex + static_cast<size_t>(std::max(1, p_.holdBars)))
            return Signal::Sell;
        return Signal::Hold;
    }

private:
    DipReversionParams p_;
    std::vector<double> z_;
};

} // namespace trader
