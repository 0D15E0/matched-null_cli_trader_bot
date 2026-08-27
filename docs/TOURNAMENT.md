# `tournament`: breeding a strategy population, and what it found

This document covers the `tournament` subcommand, the twenty-four strategy
families it searches, and the result of running it across twelve environment
sets. The short version of the result is at the bottom of this section and the
rest of the document is the evidence for it.

> **Nothing here cleared out-of-sample validation.** As with the rest of this
> repo, do not trade any of it. What follows is a measurement, and most of what
> it measures is how easy it is to produce a number that looks like an edge.

## What the command does

The loop is the obvious one — evaluate a population, keep what clears a
threshold, breed the survivors, discard the rest, repeat — with three decisions
underneath it that determine whether the output means anything.

### 1. What "makes more than X" is measured on

Not total return, and not Sharpe. On eleven years of an asset that rose 280x,
any long/flat rule posts a huge total return and a respectable Sharpe, and
selecting on either selects for market beta the strategy did not create. The
threshold is applied to

    fitness = min over environments of
                ( excess annualized Sharpe over buy & hold
                  - (max(0, drawdown - tolerance) / tolerance)^2 )

which is `evolution::combinedFitness`, unchanged from the rest of the repo. The
minimum across environments means a candidate is only as good as its worst
market and cannot win by specializing.

### 2. Where it is measured

The last 30% of every series (configurable via `--train-frac`) is sliced off
before generation zero and opened exactly once, on the finalists, at the end.
The holdout window carries an indicator warm-up prefix drawn from the training
side, so the strategy is not sitting blind for eighty bars while buy-and-hold is
invested from the first one.

### 3. What the survivors are compared to

Two **control families** ride along in the population and are bred, mutated and
selected exactly like everything else:

- `control_random` — coin-flip entries at an evolvable probability, with an
  evolvable holding period. Its randomness is a hash of (seed, bar timestamp)
  rather than a stateful RNG, because the `Strategy` contract requires that
  re-evaluating a bar return the same signal, and because a control whose
  results move between runs cannot be compared to anything.
- `control_always_long` — buy the first bar, never sell. Run through the same
  engine this is buy-and-hold plus one round trip of fees, so its excess Sharpe
  should sit a hair below zero by construction. It reads **-0.00** on every
  instrument tested, which is the arithmetic check that the benchmark inside
  `BacktestReport` measures what it claims to.

`control_random` is the **empirical null**. The best train fitness any control
reaches is a direct measurement of what this tournament's selection pressure
extracts from candidates that cannot work, and any survivor at or below that
level is marked `<- below null` in the report. This is reported alongside the
deflated Sharpe rather than instead of it, because the two answer the same
question under different assumptions: the deflated Sharpe assumes the trials
were independent and normally distributed, and a population of mutated
survivors is neither.

## Two phases, and why

The first version of this command had one phase, and it was wrong in a way
worth recording.

With a single phase, survival is decided by X from generation zero. On four
crypto instruments exactly one family — `squeeze_breakout` — cleared X at its
*published default parameters*, so from generation two onward it held every
breeding slot, and the run finished reporting its dominance. But what had
actually been measured was which family happened to start closest to the
threshold. The other twenty-one were compared against a tuned opponent while
still sitting at their untuned defaults. Adding speciation did not fix it,
because splitting breeding capacity across *surviving* families does nothing
when only one family survives.

So the run is now split:

- **Phase 1, tuning** (`--tune-generations`, default half the run): X is not
  applied. Every family keeps its own best few and breeds them. Every idea gets
  the same budget to be adapted.
- **Phase 2, selection**: X applies. Survivors replicate, everything else is
  discarded.

X is therefore applied to *tuned champions*, which is the comparison the
question actually asks for.

## Replication is structural, not just numeric

Besides its family's parameters, every individual carries two structural genes
that mutation can change:

- a **regime gate** — none, Hurst, permutation entropy, Hawkes intensity, or
  variance ratio — wrapped around the strategy, suppressing entries (never
  exits) when the gate's statistic says the strategy's hypothesis is not
  currently supported;
- a **volatility target** drawn from a short ladder `{off, 0.10, 0.15, 0.20,
  0.30, 0.40}`.

The vol target is a discrete gene rather than a continuous one on purpose.
Sizing is the highest-leverage knob in the engine, and a continuous search over
it will find the exact fraction that flatters one particular drawdown path. A
short ladder makes the choice legible: either the candidate needs sizing to
survive or it does not.

Crossover is restricted to within a family, because parameter vectors of
different families index different quantities — a crossover between a Donchian
and an RSI rule produces a genome that is a member of neither while still
carrying one of their names.

## The families

`list-strategies` prints all of them with their searchable parameter ranges.
The bounds are part of the design, not boilerplate: a search allowed to set an
ATR stop to 0.01 will find candidates that "work" by degenerating into
something else while the report still calls them by the family's name.

### Already in this repo

`odiseo`, `sma_cross`, `pure_ichimoku`, `tsmom`, `darvas`, `patterns`,
`fib_ichimoku`, `pencil_extrap`.

### Added for this work

| family | idea | source |
|---|---|---|
| `faber_ma` | hold above your own N-period moving average, cash below | Faber (2007), *J. Wealth Management* |
| `kama_trend` | smoothing constant driven by the efficiency ratio, so the filter is fast in trends and frozen in chop | Kaufman, *Smarter Trading* (1995) |
| `ehlers_trend` | two-pole Butterworth ("SuperSmoother") slope, in units of the instrument's own volatility | Ehlers, *Cybernetic Analysis* (2004) |
| `macd_norm` | EMA spread double-normalized, first by price volatility then by its own | Baz, Granger, Harvey, Le Roux & Rattray (2015), SSRN 2695101 |
| `donchian` | new N-bar high in, new M-bar low or ATR trailing stop out | Donchian's 4-week rule; the Turtle system |
| `squeeze_breakout` | arm on a bandwidth contraction percentile, take the first close above the band | volatility clustering (Engle 1982) read as a rule |
| `rsi_reversal` | buy short-horizon weakness above a long trend filter | Jegadeesh (1990), Lehmann (1990); Connors & Alvarez (2008) |
| `ou_score` | fit an OU process to detrended log price by AR(1), trade the standardized distance from equilibrium, reject fits whose half-life is too slow | Avellaneda & Lee (2010), *Quantitative Finance* 10(7) |
| `skew_reversal` | buy after realized skewness goes sharply negative | Amaya, Christoffersen, Jacobs & Vasquez (2015), *JFE* 118(1) |
| `fracdiff` | fixed-width fractional differentiation of log price; `d` near 1 makes the signal momentum, `d` near 0 makes it mean reversion, and the search picks | Lopez de Prado, *AFML* (2018) ch.5 |
| `cusum_tb` | CUSUM event sampling plus a profit target / stop / vertical barrier exit | Lopez de Prado, *AFML* (2018) ch.2-3 |
| `vol_managed` | stand aside when realized variance is high in its own trailing distribution | Moreira & Muir (2017), *Journal of Finance* 72(4) |
| `vr_switch` | measure VR(q); run a trend rule above 1, a reversal rule below 1, hold cash in the indeterminate middle | Lo & MacKinlay (1988); Lo (2004) adaptive markets |
| `lppls_bubble` | fit a log-periodic power law singularity to log price, stand aside while a positive bubble qualifies | Johansen, Ledoit & Sornette (2000); Filimonov & Sornette (2013) |
| `control_random` | coin flips | control |
| `control_always_long` | buy and hold | control |

### Regime gates

`hurst` (already in the repo), plus `pe` (Bandt & Pompe 2002 permutation
entropy — trade only when the recent ordinal dynamics are structured enough to
justify believing a rule), `hawkes` (Hawkes 1971; Bacry, Mastromatteo & Muzy
2015 — an exponential-kernel self-excitation intensity of large moves, gating
out entries in the middle of a shock cluster), and `vr` (Lo-MacKinlay variance
ratio, which asks the same question the Hurst gate does but with a known null).

### A note on `lppls_bubble`

This is the one that needed real work to be affordable, and the trick is worth
recording. Sornette's model is

    ln p(t) = A + B (t_c - t)^m + C (t_c - t)^m cos(omega ln(t_c - t) - phi)

The nine-parameter nonlinear fit is famous for returning a different answer
every time it is run. Filimonov & Sornette's reduced form expands the cosine
into `C1 cos + C2 sin`, which removes the phase from the nonlinear part and
leaves `(A, B, C1, C2)` **linear** given `(t_c, m, omega)`. So the fit becomes a
coarse grid over three parameters with a closed-form 4x4 solve at each point.

The second observation is what makes it fast: `t_c` is parameterized relative to
the end of the fitting window, so for a fixed `(window length, m, omega, t_c
offset)` the basis vectors — and therefore `X'X` and its inverse — are
**identical no matter where in the series the window sits**. Only `X'y` changes.
Precomputing them turns each refit from 81 least-squares problems with O(w)
transcendental evaluations into 81 sets of three dot products. Combined with
`SSE = y'y - beta'X'y` from prefix sums, a full pass over 25,000 BTC 4h bars
went from ~30 seconds to 0.08, bit-for-bit identical output.

A fit counts as a bubble only if it passes the literature's qualification
filters: `0.1 <= m <= 0.9`, `6 <= omega <= 13`, `B < 0` (accelerating upward),
and the Bothmer-Meister damping condition `m|B| / (omega|C|) >= 0.8`.
Confidence is the fraction of window scales whose best fit qualifies.

### `pencil_extrap` and the cost gate

`pencil_extrap` runs an SVD-based matrix-pencil fit per bar over a 256-bar
window, which is O(n·W³) — roughly a thousand times every other family here. A
default tournament excludes it and **says so**, rather than quietly carrying it
or quietly dropping it:

```
Excluded on cost: pencil_extrap (weight 500.00 > --cost-limit 50.00).
Run it with --only pencil_extrap to search it anyway.
```

Naming a family in `--only` overrides the limit, because an explicit request is
not a mistake.

## Usage

```sh
./build/cli_trader tournament \
    --envs BTC_USDT:14400,ETH_USDT:14400,LTC_USDT:14400,XRP_USDT:14400 \
    --population 260 --generations 20 --tune-generations 12 \
    --survive-threshold 0.0 --train-frac 0.70 \
    --save tournament_results/crypto4h.json
```

Any registry family can also be run directly, with parameter overrides:

```sh
./build/cli_trader backtest --symbol BTC_USDT --period 14400 \
    --strategy fracdiff --sparams "d=0.62,zWindow=180,entryZ=0.35"
```

An unknown parameter name, or one outside the searchable range, is a hard error
rather than a silent no-op — a typo'd parameter that quietly does nothing
produces a report that looks like a measurement of something it is not.

## Results

Twelve environment sets, each a full tournament of 260 individuals over 20
generations (12 tuning, 8 selecting), around 4,400 distinct genomes evaluated
per run and roughly 53,000 in total. `--survive-threshold 0.0` throughout: to
survive, a candidate had to beat buy-and-hold on risk-adjusted return, in its
worst environment, after a quadratic drawdown penalty.

Two things are being measured. **rho** is the Spearman rank correlation
between what the search maximized (train fitness) and what the holdout
delivered, across the tuned family champions — reported over all families and
over the better half separately. **Control OOS rank** is where the coin-flip
control landed on the holdout among all family champions; rank 1 means nothing
in the pool generalized better than random entries.

| environment set | genomes | rho (all) | rho (better half) | control OOS rank | families with positive OOS |
|---|---|---|---|---|---|
| crypto 4h x4 (split 50/50) | 4505 | **+0.66** (p=0.002) | +0.42 (p=0.235) | 5/22 | **0**/21 |
| crypto 4h x4 (split 60/40) | 4465 | **+0.68** (p=0.001) | +0.48 (p=0.167) | 2/22 | **0**/21 |
| crypto 4h x4 (split 70/30) | 4527 | **+0.73** (p=0.000) | +0.20 (p=0.580) | 1/22 | **0**/21 |
| crypto 4h x4 (split 80/20) | 4567 | +0.35 (p=0.128) | +0.62 (p=0.058) | 4/22 | **0**/21 |
| crypto 15m x2 | 4604 | **+0.47** (p=0.034) | +0.44 (p=0.201) | 16/22 | **0**/21 |
| multi-asset ETF x5 (60/40) | 4414 | +0.08 (p=0.744) | +0.28 (p=0.456) | 1/20 | **0**/19 |
| multi-asset ETF x5 (70/30) | 4354 | +0.22 (p=0.354) | -0.03 (p=0.950) | 4/20 | **0**/19 |
| US sector ETF x5 | 4333 | -0.04 (p=0.861) | -0.20 (p=0.611) | 1/20 | **0**/19 |
| US megacap equity x4 | 4597 | -0.42 (p=0.066) | +0.43 (p=0.217) | 7/21 | **0**/20 |
| cross-domain x4 | 4465 | +0.41 (p=0.078) | -0.40 (p=0.289) | 1/20 | **0**/19 |
| BTC 4h alone | 4346 | **+0.77** (p=0.000) | +0.43 (p=0.219) | 7/22 | **6**/21 |
| ETH 4h alone | 4294 | **+0.44** (p=0.049) | +0.38 (p=0.279) | 16/22 | **13**/21 |

Bold marks p < 0.05 on a 20,000-iteration permutation test.

### 1. Ten multi-instrument tournaments, zero survivors

In every one of the ten runs that required a candidate to work on more than one
instrument, **not a single one of the ~20 tuned family champions had positive
holdout excess Sharpe in every environment**. Not the repo's own strategies,
not the fourteen imported from the literature, not any of their gated or
vol-targeted descendants. Across four different train/holdout split points on
the same crypto universe, the count is 0, 0, 0, 0.

The two exceptions in the table are the single-instrument runs, where "every
environment" is one environment, and section 3 is about what happened when
that result was tested properly.

Read the zero as a bound, not a proof of zero. Survival demanded positive
excess Sharpe in every environment SIMULTANEOUSLY - an AND across 4-5
estimates each carrying roughly +/-0.35 of standard error on a ~3-year
holdout. A strategy with a true +0.2 edge everywhere would clear that bar only
about a quarter of the time, so across ~20 families a field of true +0.2 edges
would still have produced several survivors; a field of +0.05..0.10 edges would
produce roughly what was observed. The honest statement is therefore: these
tournaments rule out edges of roughly +0.2 Sharpe and larger in this family
space, and are not powered to distinguish small positive edges from zero.

### 2. Selection transfers, but only where it does not matter

`rho (all)` is positive and significant in six of twelve runs. Read on its own,
that says the search works: what it liked in-sample, it liked out-of-sample.

`rho (better half)` is **not significant in any of the twelve** — p ranges from
0.058 to 0.950, and it is negative in three. Restricted to the candidates
anyone would actually consider deploying, the in-sample ordering carries no
information about the out-of-sample ordering.

So the first column is real, and it is real for an uninteresting reason: the
search reliably identifies which ideas are hopeless. `fib_ichimoku`, `darvas`,
`ou_score` and `vr_switch` are bad in-sample and bad out-of-sample, and a rank
statistic over the whole field is mostly measuring that. Between `fracdiff`,
`squeeze_breakout`, `lppls_bubble` and `tsmom` — the ones that survive — the
in-sample ranking is noise.

That is a sharper statement than "the search overfits", and it is the reason
the report now prints both numbers with the instruction to read the second one.

### 3. The one candidate that looked real, and what happened to it

On BTC 4h alone, `lppls_bubble` — the Sornette log-periodic bubble detector,
new to this repo — came first out of 22 families on **both** train fitness
(0.893) and holdout (excess Sharpe +0.55). Two independent searches, on BTC and
on ETH, over ~4,300 candidates each, converged to nearly identical parameters:

| | window | refitEvery | confThreshold | trendWindow |
|---|---|---|---|---|
| fitted on BTC | 185 | 9 | 0.527 | 264 |
| fitted on ETH | 184 | 31 | 0.472 | 267 |

Independent convergence on the window and trend length is not nothing, so it
was worth testing rather than dismissing.

**Test 1 — is it just holding less?** A long/flat rule at 50% exposure in a
choppy market can post a good Sharpe with no timing skill at all. So the
control was re-run at *matched exposure and turnover* on the identical BTC
holdout window, 200 seeds:

```
exposure-matched random control on the BTC holdout window, n=200 seeds
  mean exposure 50.8%   mean trades 96      (LPPLS: 50.5%, 97)
  excess Sharpe: mean -0.684  sd 0.448
    p50 = -0.640   p90 = -0.150   p95 = -0.040   p99 = +0.200
  max over 200 seeds = +0.220

  LPPLS on this window: excess Sharpe +0.310
  matched random controls that did at least as well: 0/200   (p = 0.005)
```

It passed. Random timing at the same exposure *lost* 0.68 of Sharpe on average
on this window, so the result is not an exposure artifact.

**Test 2 — does it transfer?** The BTC-fitted genome was then applied,
unchanged, to fourteen instruments' holdout windows — including ADA, SOL, DOGE
and TRX, which appeared in no tournament at all:

```
BTC +0.31   ETH +0.51   LTC -0.66   XRP -1.07   ADA +0.42   SOL +0.06   DOGE +1.24
TRX -0.85   SPY -0.11   QQQ +0.20   GLD +0.26   TLT -0.63   EEM -0.35   AAPL -0.08

  positive: 7/14        sign test p = 1.000
  mean excess Sharpe -0.054  sd 0.619  standard error 0.165   t = -0.32
```

Seven up, seven down, mean indistinguishable from zero. It does not transfer.

One fairness caveat on the design, recorded after the fact: the genome's
windows are denominated in BARS (window=185, trendWindow=264), so on 4h crypto
they span ~31 and ~44 days while on the six daily-equity legs they span ~9 and
~13 months - a different horizon than the one fitted. The within-crypto legs
are the fair test: 5 of 8 positive, mean +0.00. The conclusion (no transfer)
stands, but it rests on the 8 crypto legs, not the headline 14.

The deflated Sharpe agrees, and had said so already: selected from 4,346
candidates, the Sharpe expected from luck alone on that window was 1.967 — more
than the strategy's 1.31 — and the deflated Sharpe came out at 0.208 against a
0.95 bar.

The lesson is specific and worth keeping. **Beating a matched null on one
window at p = 0.005 is not evidence of an edge when the candidate was chosen
from thousands.** The single-window p-value does not know about the search; the
transfer test does, and it says no. Any future candidate from this machinery
should be put through the same two tests, in that order.

### 4. What the controls did

`control_always_long` reports excess Sharpe **-0.00** on every instrument
tested, which is what buy-and-hold minus one round trip of fees should be, and
confirms the benchmark inside `BacktestReport` is measuring what it claims.

`control_random` is the interesting one. On the holdout it ranked **first of
twenty-two family champions in four of the twelve tournaments**, and in the top
five in eight of them. On the multi-asset ETF universe it also reached the
highest *train* fitness of any family, real ones included — 23 tuned strategies,
~4,400 genomes, and coin flips won in-sample.

It never cleared the survival threshold X = 0.0 in any run, which is the one
piece of good news: the threshold is not passable by luck at these population
sizes. But it did not need to clear a threshold to make the point.

### 5. Where the drawdown improvement is real

One result did survive everything, and it is not a strategy. Across every run,
the vol-targeting gene was selected almost universally — the champion of nearly
every family carries `vt=0.10` to `vt=0.20` — and the holdout drawdowns are
dramatically lower than buy-and-hold's: typically 11-25% against 54-84%.

That is not an edge and should not be confused with one. It is leverage
reduction, and it costs return in proportion (the LPPLS BTC champion's excess
CAGR is -26%). But it is the one effect in this entire exercise that reproduced
in every environment, every split, and every family, which matches what the
repo's README already says about `--vol-target` being its most valuable knob.

## Look-ahead testing

`experiments/tools/causality_check` runs every registry family twice — once on
the full series and once on a truncated prefix — and asserts that every bar
before the cut produces the same signal either way. A causal strategy cannot
depend on bars that do not exist yet, so any mismatch is look-ahead.

Each bar is probed twice, flat and in-position, because a strategy's exit
branch is a different code path from its entry branch and only one is reachable
per call. This exists because the indicators here are precomputed over the
whole series in `prepare()` for speed, and that is exactly where a
forward-running window loop hides: nothing at the CLI level would notice, the
backtest would simply return a better number.

```sh
./build/experiments/tools/causality_check data/BTC_USDT_14400.ctc 0.8
```

## Honest limits of all of this

- **One holdout each, not walk-forward.** The four crypto splits are a
  robustness check, not four independent samples — they share most of their
  training data. `evolve-strategy --wf-folds` does anchored walk-forward and
  this command does not; wiring the tournament through the same folds is the
  obvious next step.
- **Long/flat only.** The engine does not short and does not size continuously.
  A signal that is genuinely informative about direction may be unable to
  express that through `Signal{Hold, Buy, Sell}`. The `fracdiff` family's
  `momentum` gene exists partly to probe this and cannot fully compensate.
- **Single-instrument sleeves, no portfolio.** Fitness is the minimum across
  environments, which is a robustness criterion, not a deployment one. Nobody
  trades their worst instrument alone. A combined-sleeve portfolio metric
  (the `portfolio` command already builds one) would be a fairer final measure
  and is not yet wired in.
- **The finalists are still chosen by train fitness.** Even the holdout
  columns carry the selection of ~12 survivors out of ~4,400, which is why the
  deflated Sharpe and the empirical null are printed next to them.
