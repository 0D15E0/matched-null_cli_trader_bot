#pragma once
#include <array>
#include <vector>

// Multi-timescale volatility FORECASTING, as an input to position sizing.
//
// The engine currently sizes from trailing realized volatility
// (BacktestConfig::volTargetAnnual -> indicators::rollingVolatility, see the
// sizeFraction lambda in backtest/engine.cpp). A trailing window is an
// estimate of the volatility that *has already happened*; what the sizer
// actually needs is the volatility of the bar it is about to hold. Those are
// not the same thing, and the gap between them is exploitable precisely
// because realized volatility has strong long memory - its autocorrelation
// decays like a power law rather than dying inside any one window, which is
// the one property of this data that survived every statistical control we
// ran. A trailing window throws that structure away: it weights the last
// `window` bars equally and everything before them at zero.
//
// Four models, so the improvement can be attributed rather than assumed:
//
//   Trailing      - exactly what the engine does today (this literally calls
//                   indicators::rollingVolatility), so an A/B against it is a
//                   clean single-variable comparison.
//   HAR           - Corsi's Heterogeneous Autoregressive model (Corsi 2009).
//                   Regresses realized variance on realized variance averaged
//                   over three horizons, conventionally 1/5/22 bars
//                   ("daily/weekly/monthly"). A sum of three timescales
//                   approximates the power-law memory well enough to be the
//                   literature's workhorse, while staying a plain OLS fit.
//   PencilHAR     - the same regression, with the three horizons MEASURED
//                   instead of assumed: the autocorrelation of the volatility
//                   proxy is decomposed into decaying exponentials by the
//                   matrix pencil method (math/pencil.h), and each recovered
//                   decay rate lambda_m becomes a horizon T_m = round(1/lambda_m).
//                   Corsi's 1/5/22 are a human calendar (a day, a trading week,
//                   a trading month) imported into a model of a market that does
//                   not observe weekends the same way on every instrument - 4h
//                   BTC bars least of all. This asks the series what its own
//                   timescales are.
//   FractionalHAR - HAR with its LONG bucket replaced by the memory shape the
//                   data actually has. Under fractional integration with memory
//                   parameter d the forecast weights decay as psi_j ~ j^(d-1),
//                   a power law, not a flat 22-bar average; `order-spectrum`
//                   measured BTC's volatility autocorrelation as k^-0.237, i.e.
//                   d ~ 0.38, six times more persistent at 141 bars than an
//                   exponential-memory control. So the two SHORT buckets are
//                   kept as they are (over a handful of bars nothing
//                   distinguishes the shapes, and flat buckets are less noisy)
//                   and only the third regressor changes, to a power-law
//                   weighted average whose exponent is estimated causally from
//                   the fit window. Isolating the change to the one term the
//                   data has something to say about is what keeps it an
//                   attributable A/B against HAR rather than a new model.
//
// FORECAST HORIZON. `horizonBars` (H) sets what is being forecast: out[i] is
// the predicted AVERAGE per-bar volatility over bars i+1 .. i+H. H=1 is the
// next-bar forecast and is bit-identical to the pre-horizon implementation.
//
// H exists because a one-bar forecast optimises the horizon that matters
// least. Measured on BTC 4h, the R^2 of a trailing-30 estimate against realized
// volatility over the next h bars runs 0.133 (h=1), 0.435 (h=20), 0.426 (h=50),
// 0.390 (h=141), 0.353 (h=300) - h=1 is the WORST horizon there is, because a
// single squared return is almost all sampling noise. Meanwhile tsmom holds a
// BTC position for 141 bars on average and odiseo for 68. A model tuned to
// predict bar i+1 and then used to size a trade held through bar i+141 is
// solving the wrong problem, which is the most likely reason a 46% gain in
// one-bar forecast MSE did not show up in Sharpe on any of six instruments.
//
// WHAT THE MEASUREMENTS SAY (QLIKE, the loss that is robust to a noisy
// variance proxy; see the notes in the .cpp for method):
//
//   * THE HORIZON IS THE HEADLINE. HAR's advantage over Trailing grows by
//     roughly eight times between H=1 and the horizon these strategies
//     actually trade:
//                        H=1      H=20     H=141
//         BTC 4h        -7.4%    -30.6%   -57.8%
//         AAPL daily    -2.8%    -30.7%   -59.1%
//         GARCH(1,1)   -48.2%    -67.8%   -78.4%
//     A one-bar target was measuring the one thing a volatility model is worst
//     at and the strategies care about least. At H=141 HAR also wins on plain
//     log MSE (BTC 0.676 vs 0.920), which it does NOT at H=1 - averaging H
//     squared returns damps the chi-square noise that makes the raw metric
//     misleading in the first place.
//
//   * FractionalHAR does NOT beat HAR, except on AAPL at H=141 (-2.5% QLIKE).
//     On BTC it loses by more as the horizon grows (+1.3%, +6.1%, +12.4%).
//     The cause is identified rather than guessed: pinning d across 0.10-0.45
//     makes BTC monotonically WORSE with more memory, so this is the model
//     being wrong for BTC and not the estimated d being noisy. The control
//     that settles it is plain HAR with a widened long bucket - on BTC at
//     H=141, QLIKE runs 0.346 / 0.368 / 0.407 / 0.484 for long buckets of
//     22 / 50 / 100 / 187 bars. A longer long-bucket simply hurts on BTC, for
//     any shape. What the power law does earn is the comparison at EQUAL span:
//     0.414 against the flat 187-bar bucket's 0.484 on BTC, and -3.9% against
//     -0.2% on AAPL. The memory shape is right; the term it replaces was
//     already the better regressor.
//
//   * PencilHAR does NOT beat HAR either (BTC +4.0% QLIKE at H=1), because the
//     matrix pencil finds exactly ONE decaying mode in these series, leaving a
//     one-horizon HAR to compete with a three-horizon one. What it does
//     establish is that the horizon it finds is meaningful: AAPL daily lands on
//     22-30 bars, Corsi's "monthly" term measured rather than assumed, and BTC
//     4h on 43-84 bars (7-14 days), a timescale no trading calendar suggests.
//
//   * The memory parameter is real and it is measurable. Estimated causally,
//     BTC 4h gives d = 0.380 over the full series - the same 0.38 that
//     `order-spectrum` reports from a completely different estimator. AAPL is
//     poorly determined exactly as expected at 2512 bars (full-series 0.100,
//     rolling mean 0.223 with sd 0.149 over only 31 usable windows), and an iid
//     control correctly yields NO usable estimate at all.
//
//   * Cost per 25k bars: Trailing 0.7ms, HAR 13.8ms, FractionalHAR 64.6ms,
//     PencilHAR 382ms. horizonBars does not affect cost. Prefer HAR inside the
//     evolutionary search; raise refitEvery for the other two.
//
// CAUSALITY. Every value returned here is a forecast made AT bar i for bar
// i+1 and depends only on close[0..i]. That includes the OLS coefficients
// (rolling window ending at or before i), the log-space rescaling constants,
// and - for PencilHAR - the discovered horizons themselves. There is no
// full-sample anything in forecastVolatility(). A single look-ahead in a
// volatility forecast is not a small error: it manufactures an edge that
// survives every downstream test, because knowing tomorrow's volatility is
// enough to know when to be small. vol_forecast's test suite gates on an
// explicit truncation test (recompute from close[0..i], require bit-level
// agreement with the full-series run at i) for exactly this reason.
namespace trader::indicators {

enum class VolModel { Trailing, HAR, PencilHAR, FractionalHAR };

struct VolForecastConfig {
    VolModel model = VolModel::Trailing;
    int trailingWindow = 30;                     // Trailing only
    std::array<int, 3> harWindows = {1, 5, 22};  // HAR; PencilHAR overwrites;
                                                 // FractionalHAR keeps [0] and [1]
                                                 // and replaces [2] with the
                                                 // power-law weighted average
    int fitWindow = 750;                         // rolling OLS window (bars)
    int minFitSamples = 250;                     // no forecast before this many
    int pencilAcfLags = 60;                      // ACF length for timescale discovery
    int pencilModes = 3;
    int refitEvery = 50;                         // refit OLS (and rediscover) every N bars

    // Forecast horizon in bars. out[i] predicts the AVERAGE per-bar volatility
    // over bars i+1 .. i+H. Applies to EVERY model. H=1 reproduces the
    // pre-horizon behaviour exactly, for all models, bit for bit.
    int horizonBars = 1;

    // FractionalHAR memory parameter. Negative (the default) means estimate it
    // causally from each rolling fit window; a value in (0, 0.5) pins it.
    // Ignored by the other models.
    double fractionalD = -1.0;
};

// Forecast volatility (per-bar standard deviation of returns) for every bar.
// out[i] is the forecast made AT bar i covering bars i+1 .. i+horizonBars, and
// MUST use only close[0..i]. NaN before warm-up.
//
// With the default horizonBars = 1 this is the next-bar forecast. For H > 1 it
// is the average per-bar volatility over the next H bars - still a per-bar
// standard deviation, so the engine's sqrt(barsPerYear) annualization and the
// meaning of --vol-target are unchanged, and only the question being asked
// moves.
//
// The unit is deliberately the same as indicators::rollingVolatility's: a
// per-bar standard deviation of returns, which engine.cpp annualizes by
// sqrt(barsPerYear). The regression models forecast a *variance* and are
// mean-unbiased in variance before the sqrt (see the smearing correction in
// the implementation) - without that, swapping in HAR would silently move the
// meaning of --vol-target by a factor of ~2 and the A/B would be measuring
// leverage, not forecasting.
//
// Fallbacks, in order: before `minFitSamples` usable regression samples exist
// the result is NaN (the engine already treats a NaN volatility as "risk not
// measurable yet -> skip the entry", which is the correct conservative
// behaviour). After that, any bar whose fit is unusable - singular normal
// equations, degenerate all-flat data, a non-finite forecast - falls back to
// the TRAILING estimate rather than to NaN, so a numerical accident degrades
// the sizer to today's behaviour instead of halting trading.
std::vector<double> forecastVolatility(const std::vector<double>& close,
                                       const VolForecastConfig& cfg);

// Horizons PencilHAR discovered on the whole series - for reporting only,
// never for sizing. Empty for other models.
//
// "Empty" is {0, 0, 0}: the return type is fixed by the engine-side contract,
// so a zero marks an absent horizon (0 is not a legal averaging window). This
// function is the ONE place in this file that is allowed to look at the whole
// series at once, because its output is a research artifact - it answers "what
// timescales does this instrument have?" for a report. forecastVolatility()
// never calls it and never sees its result.
std::array<int, 3> discoveredHorizons(const std::vector<double>& close,
                                      const VolForecastConfig& cfg);

// The fractional memory parameter d estimated over the whole series, by the
// same estimator FractionalHAR runs on each rolling fit window. Reporting only,
// exactly like discoveredHorizons above: forecastVolatility() never calls it.
// NaN when the estimate is degenerate, which is a real answer and should be
// reported as one rather than replaced with a number - a series that cannot
// pin down its own memory (AAPL daily, at 2512 bars) is telling you that
// FractionalHAR has nothing to work with there.
//
// Call it on a prefix close[0..i] to see the value the sizer would actually
// have used at bar i; that is how the drift figures in the notes were measured.
double estimatedMemoryD(const std::vector<double>& close,
                        const VolForecastConfig& cfg);

} // namespace trader::indicators
