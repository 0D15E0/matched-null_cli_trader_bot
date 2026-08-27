#pragma once
#include "../core/candle.h"
#include "../indicators/vol_forecast.h"
#include "../strategy/strategy.h"
#include "metrics.h"
#include <cstddef>
#include <string>
#include <vector>

namespace trader {

struct TradeRecord {
    int64_t entryTime = 0;
    int64_t exitTime  = 0;
    double  entryPrice = 0;
    double  exitPrice  = 0;
    double  units      = 0;
    double  fees       = 0;  // entry + exit fees, quote currency
    double  pnlPct = 0;      // net of both fees, relative to total cash outlay
    double  pnlAbs = 0;      // net of both fees
    bool    forcedClose = false; // closed by end-of-data, not by a signal
    bool    shortSide = false;   // true when this trade was a short (long/short mode)
};

// When an order produced by bar i's signal is assumed to execute.
//
//   NextOpen  - at bar i+1's open. This is the honest default: a signal
//               derived from bar i's close cannot be acted on until that
//               close has happened, so the earliest realistic fill is the
//               next bar. It is also what the live loop actually does.
//   SameClose - at bar i's close, i.e. at the very price that triggered the
//               signal. Kept only so the optimism of the old behaviour can
//               be measured against the realistic one; it flatters
//               breakout/momentum strategies precisely on the fast bars
//               where their P&L is concentrated.
enum class FillTiming { NextOpen, SameClose };

struct BacktestConfig {
    double startingEquity = 1000.0;
    double feePct = 0.0015;      // 0.15%, roughly a crypto taker fee
    double slippagePct = 0.0005; // fixed adverse price move on every fill
    FillTiming fillTiming = FillTiming::NextOpen;

    // Volatility targeting. 0 disables it and every entry commits the whole
    // account (the original behaviour). Otherwise the position is sized so
    // that, at the entry bar's realized volatility, the account runs at
    // roughly `volTargetAnnual` annualized volatility - the standard way
    // trend-following is made investable, and the reason its drawdowns are
    // survivable in practice. Sizing is applied at entry and held for the
    // life of the trade (no intra-trade rebalancing, which would add
    // turnover the fee model would then have to carry).
    // When vol targeting is on and the volatility estimate is not yet warm,
    // the entry is SKIPPED rather than sized. Falling back to the position cap
    // (as this first did) means the least-informed bars get the LARGEST
    // position - and because a walk-forward test window restarts the warm-up,
    // that mis-sized entry was landing on the first trade of every fold and
    // compounding into the whole out-of-sample equity curve.
    double volTargetAnnual = 0.0;
    int    volWindow = 30;
    double maxPositionFraction = 1.0; // hard cap; the engine does not borrow

    // How the volatility used for sizing is estimated. Trailing reproduces the
    // original behaviour exactly (a rolling standard deviation of past
    // returns); HAR forecasts the volatility of the bar about to be HELD,
    // which is what the sizer actually needs. See indicators/vol_forecast.h -
    // on a GARCH series with known truth, HAR cuts forecast MSE by 46%.
    // volWindow above feeds the Trailing model, so the A/B stays single-variable.
    indicators::VolForecastConfig volForecast;

    // Bars before this timestamp are used to warm up indicators but are not
    // traded, not marked into the equity curve, and not part of the benchmark.
    // Walk-forward test windows need this: slicing a fold at the boundary
    // leaves every indicator cold, so the strategy sits blind for its warm-up
    // (77 bars for odiseo) while buy-and-hold is invested from bar one - which
    // silently biased every out-of-sample comparison against the strategy.
    // 0 disables it and the whole series is evaluated.
    int64_t evaluateFromTimestamp = 0;

    // LONG/SHORT MODE. Off by default: the engine stays the long/flat
    // simulator every published number came from. When on, the Signal
    // vocabulary is reinterpreted symmetrically - Buy targets LONG, Sell
    // targets SHORT, Hold keeps the current side - so the strategy is
    // always-in after its first signal (the academic TSMOM convention).
    // There is no way to express "flat" with three signals, and inventing a
    // fourth would touch every strategy in the repo; document, don't widen.
    //
    // Shorts are modelled as USDT-margined perpetual positions at 1x:
    // notional never exceeds equity (no leverage, same as the long side),
    // entry/exit pay the same fee and adverse slippage as longs, equity is
    // marked cash - units*price, and drawdown uses the bar HIGH (the worst
    // intrabar point for a short). Funding/borrow is a constant annualized
    // rate charged per bar on open notional: shortCostAnnual on shorts,
    // longCostAnnual on longs. Positive = the position pays, negative = it
    // earns. Poloniex keeps only ~180 days of funding history (and its docs
    // misstate both the timestamp unit and the limit cap - see
    // scripts/record_funding.py, which preserves the data daily), so rates
    // beyond that window are scenario inputs, stated in the report, not data.
    //
    // Known reporting wrinkles, found by adversarial review (2026-08-26) and
    // deliberately documented rather than papered over:
    //   * Funding flows through cash into equity/Sharpe/CAGR/drawdown (verified
    //     to the cent) but is NOT attributed to TradeRecord.pnlAbs, so win rate
    //     and per-trade stats are pre-funding.
    //   * TradeRecord.pnlPct uses entry NOTIONAL as denominator here, vs entry
    //     outlay (incl. fee) in the long/flat loop - a ~feePct relative skew.
    //   * If equity touches <= 0 (shorts can do that; no liquidation is
    //     modelled), statsFromCurve drops returns from non-positive equity and
    //     cagrPct returns 0 - every statistic on such a curve is INVALID, not
    //     merely caveated. Treat any LS run whose drawdown approaches 100% as
    //     a liquidation, i.e. a dead account, whatever the printout says.
    bool   allowShort = false;
    double shortCostAnnual = 0.0;
    double longCostAnnual = 0.0;

    // Number of candidate strategies that were evaluated to select the one
    // being reported. Used only to compute the deflated Sharpe ratio, which
    // is meaningless without it. 0 means "not the product of a search".
    size_t searchTrials = 0;
};

// Risk/return summary of one equity curve. Computed identically for the
// strategy and for its buy-and-hold benchmark so the two are comparable.
struct PerformanceStats {
    double totalReturnPct = 0;
    double cagrPct = 0;
    double sharpeAnnual = 0;      // properly annualized, NOT a t-statistic
    double sharpeStdErr = 0;
    double sortinoAnnual = 0;
    double maxDrawdownPct = 0;      // intrabar-aware (uses bar lows)
    double maxDrawdownClosePct = 0; // close-to-close, for comparison
};

struct BacktestReport {
    std::string strategyName;
    std::string symbol;
    double startingEquity = 0;
    double endingEquity = 0;

    PerformanceStats strategy;
    PerformanceStats benchmark; // buy & hold, same fees/slippage/timing

    // What the strategy added over simply holding the asset. This is the
    // number that decides whether any of the machinery is worth running:
    // a long/flat rule on a decade of BTC will show a huge total return
    // while adding nothing over holding.
    double excessCagrPct = 0;
    double excessSharpe = 0;

    double winRatePct = 0;
    size_t numTrades = 0;
    double exposurePct = 0;   // share of bars holding a position
    double totalFees = 0;
    double avgPositionFraction = 0;

    double barsPerYear = 0;
    double spanYears = 0;

    // Probability the true Sharpe is positive after correcting for the
    // number of strategies tried (0 when searchTrials == 0).
    double deflatedSharpe = 0;
    double expectedMaxSharpeUnderNull = 0;
    size_t searchTrials = 0;

    std::vector<TradeRecord> trades;
    std::vector<double> equityCurve; // aligned with candle index

    void print() const;
    // One-line summary, for tables comparing many environments.
    std::string summaryLine() const;
};

// Long/flat bar-by-bar simulator. No shorting; optional volatility-targeted
// position sizing; orders produced by bar i execute at bar i+1's open by
// default. The strategy is consulted through the PositionContext this class
// owns, so the exact same call sequence can be replayed by LiveTrader.
class BacktestEngine {
public:
    explicit BacktestEngine(BacktestConfig config = {}) : config_(config) {}

    BacktestReport run(const CandleSeries& series, Strategy& strategy);

    const BacktestConfig& config() const { return config_; }

private:
    BacktestConfig config_;
};

// Buy-and-hold benchmark over the same series, charged the same fees,
// slippage and fill timing as a strategy trade. Exposed separately so the
// evolutionary searches can score strategies on excess performance rather
// than on raw return, which on a decade of BTC is ~90% market beta.
// Risk/return statistics from an arbitrary equity curve. Exposed so callers
// that build their own curve - notably the portfolio command, which combines
// several single-instrument sleeves into one - measure it with exactly the same
// Sharpe, Sortino, CAGR and drawdown code the single-instrument path uses,
// rather than a second implementation that can drift from it.
//
// `lowCurve` carries the intrabar low-water mark of each bar and is what makes
// the drawdown intrabar-aware. Pass the same vector as `closeCurve` to get a
// close-to-close drawdown instead; the returned maxDrawdownPct and
// maxDrawdownClosePct are then equal, which is the honest signal that no
// intrabar information went in.
PerformanceStats performanceFromCurve(const std::vector<double>& closeCurve,
                                       const std::vector<double>& lowCurve,
                                       double startEquity, double barsPerYear,
                                       double years);

PerformanceStats buyAndHoldStats(const CandleSeries& series, const BacktestConfig& config);

} // namespace trader
