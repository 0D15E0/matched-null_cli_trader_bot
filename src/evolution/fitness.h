#pragma once
#include "../backtest/engine.h"
#include <algorithm>
#include <cstddef>
#include <vector>

namespace trader::evolution {

// What the evolutionary searches are actually selecting for.
//
// The previous scoring function was `sharpeRatio - maxDrawdownPct/100`, and
// both terms were broken:
//
//   * `sharpeRatio` was a t-statistic that scaled with sqrt(bar count), so an
//     identical strategy scored ~3.4x higher on a 25,000-bar crypto history
//     than on a 2,500-bar equity one. A fitness defined as the MINIMUM across
//     environments was therefore comparing incommensurable numbers, and the
//     "weakest link" was usually just whichever environment had the fewest
//     bars. It also measured raw performance, so on two assets that rose 15x
//     and 280x, "generalize across environments" degenerated into "stay long
//     in uptrends" - the search was rewarded for market beta it did not
//     create.
//
//   * the drawdown term subtracted 0.75 for a 75% drawdown, against a Sharpe
//     term on a ~4.0 scale. Selection happily traded one point of inflated
//     Sharpe for a hundred points of drawdown, which is exactly the genome
//     the old search kept finding: 75-81% drawdowns, no stop loss.
//
// So: score on EXCESS annualized Sharpe over buy-and-hold, and penalize
// drawdown quadratically past a tolerance, which makes deep drawdowns
// disqualifying rather than mildly unattractive.
struct FitnessConfig {
    // Score alpha (strategy Sharpe minus buy-and-hold Sharpe) rather than
    // raw Sharpe. Turn off only to reproduce a legacy result.
    bool useExcessSharpe = true;

    // Drawdown (as a fraction) treated as the edge of acceptable; beyond it
    // the penalty grows with the square of the overshoot. At the defaults a
    // 50% drawdown costs 1.0 and a 75% drawdown costs 4.0, against an excess
    // Sharpe that realistically lives in [0, 1].
    double drawdownTolerance = 0.25;
    double drawdownWeight = 1.0;

    // Activity floor. A flat rule that never trades has no drawdown and a
    // meaningless Sharpe, so it must not be allowed to win by inactivity -
    // but a fixed floor of 5 trades let a decade-long environment qualify on
    // five trades, whose Sharpe standard error is as large as the estimate.
    int minTrades = 10;
    double minTradesPerYear = 2.0;
};

// Score for one (strategy, market) pair. Lower is worse; near-inactive
// genomes get a large negative score that still rewards moving toward the
// activity floor, so the search has a gradient to climb.
inline double environmentScore(const BacktestReport& r, const FitnessConfig& cfg) {
    double required = std::max(static_cast<double>(cfg.minTrades),
                               cfg.minTradesPerYear * r.spanYears);
    if (static_cast<double>(r.numTrades) < required) {
        return -50.0 + static_cast<double>(r.numTrades) / std::max(1.0, required) * 10.0;
    }

    double sharpe = cfg.useExcessSharpe ? r.excessSharpe : r.strategy.sharpeAnnual;

    double drawdown = r.strategy.maxDrawdownPct / 100.0;
    double overshoot = std::max(0.0, drawdown - cfg.drawdownTolerance);
    double normalized = cfg.drawdownTolerance > 0.0 ? overshoot / cfg.drawdownTolerance : overshoot;
    return sharpe - cfg.drawdownWeight * normalized * normalized;
}

// Fitness = the minimum environment score. A genome is only as fit as its
// worst market, so it cannot win by specializing. This part of the original
// design was sound and is kept; what changed is that the scores being
// compared are now on the same scale and measure alpha rather than beta.
inline double combinedFitness(const std::vector<BacktestReport>& reports,
                               const FitnessConfig& cfg) {
    if (reports.empty()) return -1e9;
    double worst = 1e9;
    for (const auto& r : reports) worst = std::min(worst, environmentScore(r, cfg));
    return worst;
}

} // namespace trader::evolution
