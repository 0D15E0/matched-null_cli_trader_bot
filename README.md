# cli_trader

A backtesting-focused C++ CLI, re-architected from the legacy Qt GUI projects
`Console/PM_CONSOL`, `Console/PM2*`, and `PortfolioManager/` in this workspace.
No GUI, no Qt dependency — a small, fast native binary.

> **New here?** [STRATEGY.md](docs/STRATEGY.md) explains what the live book does
> and why, in plain English. This README covers the machinery and its history.
>
> **Working on the code?** [ARCHITECTURE.md](docs/ARCHITECTURE.md) maps the layers,
> processes, threads and state machines, with diagrams.
>
> **Running it?** [RUNBOOK.md](docs/RUNBOOK.md) is the operations manual.
> [TOURNAMENT.md](docs/TOURNAMENT.md) is the strategy-search write-up.

## What the live book actually runs

Not a single rule — a **majority vote of three**. The live strategy is
`ensemble_vote`, running eight sleeves (BTC, ETH, XRP, LTC, DOGE, TRX, ADA, SOL
against USDT) on 4h candles. Each member answers one question about trend, each
reads that trend through a genuinely different statistic, and each runs at its
published defaults:

| rule | in plain terms | age of the idea |
|---|---|---|
| Momentum (`tsmom`) | "Is the price higher than it was 15 days ago, by more than 5%?" | academic standard since the 1980s–2012 |
| Moving average (`faber_ma`) | "Is the price above its own average of the last ~33 days?" | Faber 2007, and folk knowledge long before |
| Breakout (`donchian`) | "Did the price recently punch through its ~9-day high, and has it not broken down since?" | Donchian's rule, 1960s; the Turtle traders |

**The vote has hysteresis: go long when at least 2 of 3 want to be long, close
only once 1 or fewer still do.** A single member flickering at its own threshold
therefore cannot churn the book. Exits are never blocked.

Why a vote instead of just picking the best rule? Because *which* trend rule to
run turned out to be a choice the data cannot make. At their published defaults
on the same portfolio machinery, these rules scattered from Sharpe −0.68 to
+0.97 — and that ordering is not stable: it moved visibly when four days of bars
were appended, and twelve tournaments found the in-sample ranking of the *good*
rules carries no out-of-sample information (rank correlation over the better
half: never significant in 12 runs). So the response is the one the portfolio
already applies to assets: **when a choice is noise, don't make it — diversify
across it.** Averaging rules whose errors are imperfectly correlated reduces
variance by the same arithmetic that makes the multi-sleeve portfolio work. That
is a theorem doing the work, not a forecast.

Position size is `30% / (annualized volatility at entry)`, capped at 100% and
fixed for the life of the trade — vol targeting is the single most valuable knob
in this repo, and the one result here that reproduces. Cadence is roughly two
signals per sleeve per month.

Two things this is *not*. It is not an edge on top of the members: a vote of
trend rules is still a trend follower, and it inherits their exposure to
market beta. And it has not cleared walk-forward validation. What the ensemble
buys is **variance reduction on the rule-selection decision**, nothing more.

Details and the reasoning in full: [STRATEGY.md](docs/STRATEGY.md) in plain
English, `src/strategy/zoo/ensemble.h` in code. The inverse-volatility research
variant is explained in [ENSEMBLE_INVOL.md](docs/ENSEMBLE_INVOL.md), including
the fixed multi-timeframe sweep and its current development-only results.

## What the numbers actually look like

`odiseo` on BTC_USDT 4h, ~11.5 years — the run that used to be quoted as
"+3743%, Sharpe 3.16":

| | strategy | buy & hold | excess |
|---|---|---|---|
| Total return | +4302% | **+27823%** | -23521% |
| CAGR | 39.0% | **63.3%** | -24.3% |
| Sharpe (annualized) | 0.94 ± 0.30 | **1.05** | **-0.11** |
| Max drawdown | 65.1% | 84.0% | |

Buy-and-hold wins. That is the normal result here: across four symbols and four
strategies, **15 of 16 combinations lose to buy-and-hold on risk-adjusted
return**. A long/flat rule on a decade of assets that rose 15x-280x will always
show a big total return; the excess column is the only one that says anything
about the strategy.

| Symbol | Strategy | Return | B&H return | Sharpe | B&H Sharpe | Max DD | Trades |
|---|---|---|---|---|---|---|---|
| BTC_USDT 4h | odiseo | +4302% | +27823% | 0.94 | 1.05 | 65.1% | 188 |
| BTC_USDT 4h | pure_ichimoku | +1967% | +27823% | 0.84 | 1.05 | 61.6% | 450 |
| BTC_USDT 4h | sma_cross | +5597% | +27823% | 0.97 | 1.05 | 74.5% | 484 |
| BTC_USDT 4h | **tsmom** | +12289% | +27823% | **1.16** | 1.05 | 67.8% | 88 |
| ASML.AS 1d | odiseo | +224% | +1489% | 0.65 | 0.97 | 32.0% | 56 |
| ASML.AS 1d | tsmom | +836% | +1489% | 0.96 | 0.97 | 42.9% | 12 |
| AAPL 1d | odiseo | +274% | +1035% | 0.86 | 0.98 | 37.9% | 56 |
| TSLA 1d | sma_cross | +1080% | +2145% | 0.80 | 0.82 | 62.6% | 47 |

### The one configuration that beats holding

`tsmom --hurst-filter --vol-target 0.20` on BTC_USDT 4h:

| | strategy | buy & hold |
|---|---|---|
| Sharpe (annualized) | **1.34 ± 0.30** | 1.05 |
| Max drawdown | **31.9%** | 84.0% |
| Total return | +2656% | +27823% |
| Time in market | 33% | 100% |
| Avg capital deployed | 52% | 100% |

A quarter of the drawdown at a better Sharpe, using half the capital a third of
the time. Two honest caveats: **+0.29 excess Sharpe is about one standard
error**, so this is encouraging rather than established; and this combination
was chosen by looking at the data, which is its own mild selection bias.

## Build

Requires CMake ≥ 3.16, a C++17 compiler, libcurl and OpenSSL (system-provided on
macOS/Linux). `nlohmann/json` is fetched automatically via CMake `FetchContent`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Usage

```sh
# Pull public OHLCV history. Never stores the candle that is still forming, and
# re-checks a few already-stored bars against the venue before appending.
./build/cli_trader fetch --symbol BTC_USDT --period 14400 --data-dir data
./build/cli_trader fetch --symbol AAPL --period 86400 --source yahoo --range 10y

# Check a store's integrity (spacing, OHLC invariants, outliers, gaps).
./build/cli_trader validate --symbol BTC_USDT --period 14400

# Backtest. Every report includes a buy-and-hold benchmark charged the same fees.
./build/cli_trader backtest --symbol BTC_USDT --period 14400 --strategy tsmom \
    --hurst-filter --vol-target 0.20

./build/cli_trader list-strategies
```

Supported `--period` values (seconds): `300, 900, 1800, 7200, 14400, 86400` for
Poloniex; `300, 900, 1800, 3600, 86400, 604800` for Yahoo.

## How the backtest engine measures things

These are the choices that make the difference between a number you can act on
and a number that flatters you.

**Orders fill at the next bar's open.** A signal computed from a bar's close
cannot be acted on until that close has happened. `--fill-timing same-close`
reproduces the old behaviour if you want to measure how much it flattered a
strategy — interestingly, for `odiseo` on BTC it turns out to flatter it
*negatively* (Sharpe 0.89 vs 0.94), so the direction of that bias is
strategy-dependent rather than universal.

**Sharpe is annualized, and comes with a standard error.** The annualization
factor is measured from the data (`CandleSeries::barsPerYear()`) rather than
derived from the nominal period, so daily equities correctly count ~252 bars a
year instead of 365. A Sharpe from 8 trades prints an error bar as large as
itself, which is the point.

**Drawdown looks inside the bar.** Equity is marked at each bar's low, not just
its close, so a position that was deep underwater intrabar is reported that way.

**Both fees are charged to the trade.** Entry and exit fees count against trade
P&L and the win rate, and any position still open when the data ends is closed
at cost, so the total return is an amount you could have withdrawn.

**Position sizing can target volatility.** `--vol-target 0.20` sizes each entry
for ~20% annualized account volatility instead of going all-in. This is the
single most valuable knob in the repo: on `tsmom`/BTC it took max drawdown from
67.8% to 39.7% while *improving* Sharpe.

## Research protocol and benchmark updates

The repository now has two separate layers for research:

1. `dev_portfolio_sweep.py` and `dev_portfolio_walkforward.py` are fixed,
   development-only comparisons. Their boundary is `2023-12-31`; they do not
   accept an end date that could accidentally expand the search into the
   frozen 2024+ period. Walk-forward folds receive an 80-bar causal warm-up
   prefix, but those bars are not traded or scored.
2. `research_protocol.py` is the candidate registry and kill layer. Every
   hypothesis records its claim, strategy family, exact `--sparams`, volatility
   target, portfolio weighting, filter settings, metric, kill rules, commands,
   fold results, and terminal decision. A candidate runs once. Seeing a result
   and changing a parameter requires a new hypothesis ID.

The standard development folds are:

| fold | scored period |
|---|---|
| 1 | 2018-01-01 to 2019-12-31 |
| 2 | 2020-01-01 to 2021-12-31 |
| 3 | 2022-01-01 to 2023-12-31 |

The default kill rule requires at least two positive folds, mean excess Sharpe
of at least `0.10`, worst-fold excess Sharpe no lower than `-0.20`, and worst
close-to-close drawdown no higher than `40%`. `survives_development` means only
that a candidate cleared this development gate. It is not a deployment verdict.

```sh
python3 experiments/research_protocol.py new \
  --id donchian-invvol-vt020 \
  --hypothesis "Inverse-volatility sleeve weighting improves Donchian portfolio risk-adjusted return." \
  --strategy donchian --sparams "entryWindow=55,exitWindow=20" \
  --vol-target 0.20 --weights invvol --vol-lookback 120 --rebalance 30

python3 experiments/research_protocol.py validate donchian-invvol-vt020
python3 experiments/research_protocol.py run donchian-invvol-vt020
python3 experiments/research_protocol.py list
```

### What changed in the portfolio benchmark

Portfolio results now combine sleeves on a common timestamp grid and compare
them with an exact equal-weight buy-and-hold basket of the same instruments.
The basket uses the same next-open entry, fee, slippage, close marking, and
final liquidation conventions as the strategy. Portfolio exposure is capped at
100%; inverse-volatility weights, when selected, use only trailing returns and
are refreshed at the configured rebalance interval.

The benchmark accounting has a regression test in
`tests/portfolio_benchmark_test.cpp`, registered with CTest. The causal warm-up
path and the research protocol also have automated coverage:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
python3 -m unittest tests/research_protocol_test.py
```

The reported portfolio drawdown is close-to-close. Single-instrument reports
can be intrabar-aware, so those two drawdown numbers must not be compared as if
they used the same convention.

### First registered discovery batch

The initial candidates were deliberately varied before looking at their
results: signal parameters, portfolio construction, a Hurst regime filter,
adaptive trend, and mean reversion. All ran on the fixed four-asset development
protocol and were evaluated once.

| hypothesis | status | mean excess Sharpe | positive folds | worst drawdown |
|---|---|---:|---:|---:|
| `donchian-invvol-vt020` | survives development | 0.517 | 2/3 | 13.23% |
| `donchian-wide90-vt020` | survives development | 0.413 | 3/3 | 11.75% |
| `tsmom-long120-vt020` | survives development | 0.347 | 3/3 | 21.28% |
| `tsmom-hurst055-vt020` | killed | -0.323 | 0/3 | 11.74% |
| `kama-trend-vt020` | killed | -0.683 | 0/3 | 44.66% |
| `rsi-reversal-vt020` | killed | -1.207 | 0/3 | 15.22% |
| `ou-score-vt020` | killed | -1.830 | 0/3 | 41.97% |

These results are discovery evidence, not proof that the survivors have a
future edge. The complete immutable records live in
`experiments/hypotheses.json`.

### Is the deployed 4h ensemble the best tested configuration?

The deployed configuration is `ensemble_vote` with `enterVotes=2,exitVotes=0`,
eight equal 4h sleeves, `--vol-target 0.30`, and `--vol-window 90`. I checked
it against its members, the previous `exitVotes=1` hysteresis, and causal
inverse-volatility sleeve weighting using the identical eight-symbol universe:
`BTC`, `ETH`, `XRP`, `LTC`, `DOGE`, `TRX`, `ADA`, and `SOL`. The fixed window was
the common pre-2024 development history from `2021-01-01` through
`2023-12-31`; because SOL is newer, this produces 4,343 common 4h bars, about
1.98 years. The 2024+ holdout was not used.

| configuration | Sharpe | excess Sharpe | max drawdown |
|---|---:|---:|---:|
| deployed ensemble, equal sleeves, `2/0` | 0.95 | 0.87 | 20.08% |
| TSMOM only, equal sleeves | **1.02** | **0.94** | **15.52%** |
| Faber MA only, equal sleeves | 0.57 | 0.49 | 22.45% |
| Donchian only, equal sleeves | 0.32 | 0.24 | 16.47% |
| ensemble, equal sleeves, previous `2/1` exit | 0.66 | 0.58 | **19.64%** |
| ensemble `2/0`, inverse-vol sleeves | **1.11** | **1.03** | 21.37% |

**Conclusion:** the deployed equal-weight ensemble is not the best result on
this development window. The inverse-volatility ensemble is the strongest
configuration tested by Sharpe, while TSMOM alone has the lowest drawdown and
slightly better Sharpe than the deployed configuration. The advantage is not
statistically established: the portfolio Sharpe standard error on this roughly
two-year window is `0.71`, and the alternatives were inspected after the live
configuration was already known.

This is therefore a new research lead, not a live-change recommendation. The
live book remains unchanged until an exact eight-sleeve candidate is
pre-registered, tested on chronological development folds, and then judged on
the untouched forward period. No result in this section authorizes deployment
or uses the 2024+ holdout.

## Evolution, and why its old results were meaningless

`evolve` searches `OdiseoParams`; `evolve-strategy` searches the trading rules
themselves (genetic programming over a scale-invariant feature set). The
machinery was always well built. What was wrong was everything around it:

- Fitness maximized a t-statistic that scaled with sample length, so the
  "minimum across environments" was really comparing a 25,000-bar crypto series
  against a 2,500-bar equity one — incommensurable numbers.
- It scored raw performance on assets that rose 15x-280x, so "generalize across
  environments" collapsed into "stay long in uptrends."
- The drawdown penalty subtracted 0.75 for a 75% drawdown against a Sharpe term
  on a ~4.0 scale, so selection happily accepted ruinous drawdowns.
- Results were reported on the same candles fitness was maximized over, after
  trying thousands of candidates.

Now: fitness is **excess annualized Sharpe over buy-and-hold minus a quadratic
drawdown penalty**, minimized across environments, and validation is
walk-forward.

```sh
./build/cli_trader evolve-strategy \
    --envs BTC_USDT:14400,ASML.AS:86400,AAPL:86400 \
    --population 24 --generations 12 --wf-folds 3 --vol-target 0.20
```

Anchored walk-forward: fold *k* trains on everything up to a boundary and is
validated only on the window that follows. A real 3-fold run produced:

```
===== Out-of-sample verdict =====
Segments evaluated:     9
Mean excess CAGR:       -23.09%
Worst excess CAGR:      -79.14%
Beat buy & hold in:     1/9 segments

VERDICT: no out-of-sample edge over buy-and-hold. Do not deploy this.
```

— while the same genome's **in-sample** report looked strong (Sharpe 1.26 vs
buy-and-hold's 1.05, drawdown 34% vs 84%). That gap is the entire lesson of this
project, and the tool now prints it unprompted.

The verdict is judged on **excess Sharpe, not excess return**, because a
vol-targeted strategy deliberately runs at partial exposure and would lose on
raw CAGR to a fully-invested benchmark even when it is the better bet per unit
of risk. It reports a standard error across segments, requires at least six
segments, and will not call anything deployable unless the mean excess clears
two standard errors.

Test windows carry an indicator warm-up prefix that feeds the indicators without
being traded or scored. Without it, a fold sliced flush at the boundary left
`odiseo` blind for ~80 bars while buy-and-hold was invested from bar one — a
systematic bias against the strategy in the one measurement that decides
everything.

The deflated Sharpe flags overfitting independently. Every report from a search
shows:

```
Search correction (288 candidates evaluated):
  Sharpe expected from luck alone: 0.85
  Deflated Sharpe (P[true SR > that]): 0.917  <- NOT significant
```

Saved genomes carry honest provenance: how many candidates were tried, that the
genome was fitted on **all** the data, and that the walk-forward verdict judged
the *search procedure* rather than the saved artifact. A genome fitted on
everything is never stamped "validated".

Requesting validation that cannot be built is a hard error, not a silent
downgrade: a `--train-end` that leaves too little data on either side used to
hand back an essentially random genome and report it as an out-of-sample result.

> The genome files that used to be checked in (`emergent_genome*.json`,
> `emergent_bear_2022.json`, per-symbol ones) were produced by the **old**,
> broken fitness function and had no holdout. They were **deleted on
> 2026-08-26** — superseded by the `tournament` work, whose results and
> methodology live in [TOURNAMENT.md](docs/TOURNAMENT.md) and `tournament_results/`.
> They remain in git history (commit `08f689b`) if a genome is ever needed for
> forensics. `evolve-strategy --save` still writes to `emergent_genome.json` by
> default, and `backtest --strategy emergent` fails with a clear message when
> the file is absent.

## `tournament`: twenty-four families, bred and discarded

`evolve` tunes one strategy's parameters and `evolve-strategy` evolves rules
from a feature set. `tournament` does something different: it runs a
**population of whole strategy families** against each other — the eight
already here plus fourteen imported from the literature and two controls —
tunes every one of them, applies a survival threshold, breeds what clears it
and discards the rest. Full write-up in [TOURNAMENT.md](docs/TOURNAMENT.md).

```sh
./build/cli_trader tournament \
    --envs BTC_USDT:14400,ETH_USDT:14400,LTC_USDT:14400,XRP_USDT:14400 \
    --population 260 --generations 20 --tune-generations 12 \
    --survive-threshold 0.0 --save tournament_results/crypto4h.json
```

New families: Faber's 10-month rule, Kaufman's adaptive average, Ehlers'
SuperSmoother, the normalized MACD of Baz et al. (2015), Donchian/Turtle
breakouts, volatility-squeeze breakouts, short-horizon reversal, the
Avellaneda-Lee OU s-score, realized-skewness reversal (Amaya et al. 2015),
Lopez de Prado's fractional differentiation and CUSUM/triple-barrier, the
Moreira-Muir volatility-managed rule, Lo-MacKinlay variance-ratio regime
switching, and Sornette's LPPLS bubble detector. Four regime gates (Hurst,
permutation entropy, Hawkes intensity, variance ratio) and a volatility-target
ladder are **structural genes**, so a survivor can acquire a filter and a
sizing rule it was not born with. `list-strategies` prints every family with
its searchable ranges; `--sparams "d=0.62,zWindow=180"` runs any of them
directly from `backtest`.

Two of the twenty-four are controls — coin-flip entries and
buy-first-bar-never-sell — bred and selected exactly like the rest, so the
report can state what this much selection pressure extracts from candidates
that cannot work. That number is printed as the **empirical null**, next to
the deflated Sharpe.

### What it found

Twelve environment sets, ~53,000 genomes. Details and caveats in
[TOURNAMENT.md](docs/TOURNAMENT.md); the three results worth putting here:

**In all ten tournaments that required a candidate to work on more than one
instrument, zero of ~20 tuned family champions had positive holdout excess
Sharpe in every environment.** Four different split points on the same crypto
universe give 0, 0, 0, 0. Read as a bound, not a proof of zero: the
all-environments-at-once criterion is only powered to exclude edges of roughly
+0.2 Sharpe and larger - a field of small +0.05..0.10 edges would look just
like this.

**Selection transfers only where it does not matter.** The rank correlation
between train fitness and holdout performance across family champions is
positive and significant in six of twelve runs — but restricted to the better
half of the field, it is significant in **none** of the twelve (p from 0.058 to
0.950, negative in three). The search reliably identifies which ideas are
hopeless. Among the ones worth considering, its ordering is noise. The report
prints both numbers and says to read the second.

**On the holdout, coin flips ranked first of twenty-two family champions in
four of the twelve runs.** On the multi-asset ETF universe `control_random`
also posted the best *train* fitness of any family.

One candidate looked genuinely real — `lppls_bubble` came first on both train
and holdout on BTC alone, and beat 200 exposure- and turnover-matched random
controls on that window at p = 0.005. Applied unchanged to fourteen other
instruments' holdout windows it went 7 up, 7 down, mean -0.05 ± 0.17 (the fair
subset - the 8 same-timeframe crypto legs, since the genome's windows are
bar-denominated - reads 5 up, 3 down, mean +0.00). Beating a
matched null on one window is not evidence of an edge when the candidate was
chosen from 4,346; the transfer test is what carries the search correction, and
it said no.

The one effect that did reproduce everywhere is not a strategy: vol targeting
was selected by almost every champion in every run, and holdout drawdowns came
in at 11-25% against buy-and-hold's 54-84%. That is leverage reduction, it
costs return in proportion, and it is the same conclusion this README already
draws about `--vol-target`.

`experiments/tools/causality_check` verifies that every registry family gives
the same signal for a bar whether or not the bars after it exist — the
look-ahead test that precomputing indicators in `prepare()` makes necessary.

## Live trading (`run` / `status` / `parity`)

> Operating instructions — how to start the four-sleeve portfolio, what to
> monitor, what its normal behaviour looks like, and the list of what is still
> missing before real money — are in [RUNBOOK.md](docs/RUNBOOK.md).


`cli_trader run` polls fresh **closed** candles, replays every bar the strategy
has not seen yet, and places at most one order per cycle to move the account to
the position the strategy wants.

```sh
# Default: paper trading, no API keys, charged the same fee AND slippage the
# backtest charges.
./build/cli_trader run --symbol BTC_USDT --period 14400 --strategy ensemble_vote \
    --vol-target 0.30 --vol-window 90 \
    --mode paper --poll-interval 60 --data-dir data --state-dir state

./build/cli_trader status --symbol BTC_USDT --state-dir state
./build/cli_trader parity --symbol BTC_USDT --period 14400 --strategy ensemble_vote
```

The live loop used to be unable to reproduce any backtest at all. It re-ran
`prepare()` every poll, which reset each strategy's internal position flag, so
`odiseo` and `pure_ichimoku` could buy but **never sell** — no trailing stop, no
exit, unbounded drawdown. It also evaluated only the newest bar, silently
dropping every edge-triggered signal that fired while the process was down, and
it traded on the still-forming candle.

What makes parity possible now:

- **Strategies are pure functions of (series, bar, position).** The position is
  owned by the caller (`PositionContext` in `strategy/strategy.h`), so the same
  call produces the same signal in backtest, walk-forward and live.
- **The live loop replays.** Every unprocessed closed bar goes through the
  identical `observe → onBar` sequence the engine uses. A bot that was down for a
  day replays a day of bars and fires **one** trade, not a day of round trips.
- **The strategy's notional position is persisted separately from the account.**
  A trailing stop keeps its reference price across restarts, while the account
  records the real fill price.
- **Orders are idempotent.** A deterministic client order id derived from the
  decision bar (`ct-BTC_USDT-buy-1787140800`) is written to state *before* the
  order is placed, so a crash between placing and saving cannot duplicate it.
- **State writes are atomic** (temp + fsync + rename, with a `.bak`), and a
  corrupt state file raises an error instead of silently resetting to a phantom
  $1,000 balance.
- **A per-symbol lockfile** stops two `run` processes double-ordering, and
  reclaims a stale lock whose recorded pid is no longer alive.
- **Partial fills** keep the position open until the residual is dust.
- **The same risk sizing runs live.** `run` takes `--vol-target` and applies the
  identical rule the backtest uses. Without it the bot would run all-in while
  the report that justified it did not.
- **Reconciliation is conservative, and halts rather than guessing.** The bot
  never adopts base currency it did not buy — otherwise a manual holding or
  another bot's inventory gets sold the first time this strategy says Sell. On
  a material mismatch it stops trading, records `last_action:
  "halted-balance-mismatch"` with the reason in `last_error`, and keeps
  re-checking so it resumes by itself once the account agrees. A clean restart,
  where its own recorded position matches the venue, reconciles silently. Pass
  `--adopt-venue-position` to deliberately take over whatever the account holds.

`parity` closes the loop by replaying the stored candles through the backtest
engine and matching the fills it would have made against the trades actually
logged, keyed on the bar each decision came from. **The signal match rate, not
paper P&L, is the evidence that backtest results transfer.**

Real orders (`--mode live`) require `POLONIEX_API_KEY` and
`POLONIEX_API_SECRET`. The client was rewritten against Poloniex's current spot
API (unversioned — `/orders`, `/accounts/balances`; the `/v3` prefix belongs to
their separate futures API) with HMAC-SHA256 `key`/`signature`/`signTimestamp`
auth, verified against the documented worked examples and RFC 4231 vectors. It
is **still unverified against a real account** and carries a pre-flight checklist
in its header. Keys are never hardcoded.

## Indicator corrections

Several indicators disagreed with their own documentation. All are fixed, which
means **parameters tuned against the old versions no longer transfer**:

- **Ichimoku spans are now displaced forward** by `kijunWindow`, as the
  indicator is defined and as every charting package draws it. The old version
  compared price against a cloud built from the same bar's window — strictly
  causal, so not look-ahead, but a Donchian-midpoint breakout rather than
  Ichimoku.
- **ATR uses Wilder smoothing**, not an SMA of true range.
- **EMA is NaN until warm-up** and seeded with an SMA, instead of emitting from
  index 0 seeded with the first price.
- **ADX no longer NaN-poisons permanently** after a flat window, which used to
  silently freeze a strategy forever on halted or illiquid data.
- **The Hurst exponent is bias-corrected** against the Anis-Lloyd expected R/S.
  Measured on iid Gaussian returns, the old estimator returned mean H ≈ 0.56-0.59
  and passed an `H >= 0.55` "trending regime" gate **56-66% of the time on pure
  noise**. Corrected, a random walk now measures ≈ 0.47 and passes 25-30% of the
  time, while a genuinely persistent AR(1) series measures ≈ 0.59.

That last fix is what turned the Hurst filter from noise into a real filter: on
`tsmom`/BTC it now improves Sharpe 1.16 → 1.31 and cuts max drawdown 67.8% →
51.0%. The `HurstRegimeFilter` decorator was also unsound — it suppressed a Buy
*after* the inner strategy had already flipped its own position flag, leaving it
tracking a phantom position — which is why every previous `+hurst_filter`
comparison in this README measured whole-trade dropout rather than regime
filtering.

## A note on data vintage

Every number in this README was measured on the candle stores as they stood at a
particular moment, and `fetch` appends. The stores have since grown, so a fresh
run will not match to the last digit — for example the standing `tsmom` control

```sh
./build/cli_trader backtest --symbol BTC_USDT --period 14400 --strategy tsmom --vol-target 0.20
```

now reports Sharpe **1.28** where this document says 1.26, because
`BTC_USDT_14400` went from 25,170 to 25,199 bars. Adding `--end 1786795200`
reproduces 1.26 / 88 trades / 0.21 excess exactly. That is not a regression and
not a drift in the strategy: it is a longer sample. When a control number moves,
check the bar count before you check the code.

## `order-spectrum`: what order is a series' memory?

```sh
./build/cli_trader order-spectrum --symbol BTC_USDT --period 14400
```

Applies the logarithmic-spiral order estimator (`math/spiral.h`, `math/pencil.h`)
to a series' autocorrelation and asks which *model class* its memory belongs to:

- **α ≈ −1** — integer order: exponential relaxation, ordinary ARMA memory.
- **−1 < α < 0** — fractional order: power-law long memory, ACF ~ k^−(α+1).

The estimator evaluates the truncated Laplace transform on a spiral
s = r·e^{(σ+i)u} and reads the order off a matrix-pencil line spectrum. A spiral
rather than a circle because on the unit circle the transform of time-domain
data is exactly 2π-periodic, so its inverse is supported on the integers and a
non-integer order can *never* appear — for any window, sampling rate or FFT
length. Any σ ≠ 0 breaks that.

The model ω = (σ+i)·α makes both Re(ω)/σ and Im(ω) independent readings of the
same α; α is read from the least-squares projection (σ·Re + Im)/(1+σ²), which
is dramatically less noisy than Re alone (reading from Re divides by σ = 0.3,
amplifying mode noise 3.3×). The perpendicular component
β = (σ·Im − Re)/(1+σ²) is the *complex* part of the order: a real order pins
β ≈ 0 (synthetic power laws stay within |β| ≤ 0.003 across a decade of radii),
while nonzero stable β means log-periodic modulation — discrete scale
invariance. The verdict requires both radius-stability of α *and* |β| within
the reference-calibrated tolerance; a stable α with off-manifold β is reported
as a COMPLEX order, not a fractional one. Both criteria were chosen on a
pre-registered comparison: across 98 (instrument, timeframe) stores, the
projection readout's identification ratio rank-predicts tsmom excess Sharpe
better than the Re readout (Spearman −0.263 vs −0.231, permutation p = 0.031
vs 0.059).

Two things the command refuses to do, both learned by getting them wrong first:

**It will not report a single radius.** A contour at |s| interrogates timescales
t ~ 1/|s|, and exponential and power-law memory are indistinguishable at short
t. A true k^−0.3 autocorrelation reads −0.963 at radius 1 and −0.705 (correct)
at radius 0.01. `--radii` takes a sweep and requires at least two values.

**It will not report an order without controls.** The estimator returns a number
for anything, so synthetic power-law and exponential references are computed at
identical settings on every run. The statistic is not the value but whether it
**converges** as the contour reaches long timescales — `tailSprd`.

```
  series                                  r=1    r=0.3    r=0.1   r=0.03   r=0.01    spread  tailSprd  tailMean  tailBeta
  BTC_USDT                             -0.500   -0.666   -0.740   -0.739   -0.745     0.245     0.006    -0.741     0.007
  power law k^-0.3 (fractional)        -0.898   -0.785   -0.738   -0.716   -0.706     0.192     0.032    -0.720     0.001
  power law k^-0.6 (fractional)        -0.822   -0.618   -0.524   -0.470   -0.443     0.379     0.082    -0.479     0.002
  exp(-k/20) (integer)                 -0.957   -0.865   -0.668   -0.360   -0.152     0.805     0.516    -0.393     0.016
  exp(-k/200) (integer)                -0.996   -0.985   -0.957   -0.865   -0.668     0.328     0.289    -0.830     0.028
```

On BTC 4h, α = −0.741 stable to 0.006 — tighter than the true synthetic power
law at the same settings (0.082) — with β = +0.007 on the real-order manifold:
a volatility autocorrelation decaying like k^−0.26, genuine long memory rather
than exponential, and *plain* long memory rather than log-periodic. The daily
equities come back *not identified* (they have ~2,500 bars against BTC's
25,170, and their long-lag autocorrelations are too noisy to pin an order
down); the same instruments at intraday resolution move toward identification
as samples grow. A diagnostic that distinguishes "measured" from "unmeasurable"
is doing its job.

**Does the identification predict where trend following works?** Measured
across 98 (instrument, timeframe) stores — 8 Poloniex pairs × {5m…1d} ladders
at full venue depth (4.7–11.5 years, fetched after fixing a truncation bug in
the backward pagination) plus 5 equities × {5m…1d}. The full analysis is in
[experiments/](experiments/); the honest summary is that the raw headline does
not survive a correct significance test, but a sharper claim inside it does.

| statistic (n = 68 rows, ≥ 8 trades) | Spearman | p (rows independent) | p (13 instrument clusters) |
|---|---|---|---|
| identification ratio vs tsmom excess Sharpe | −0.263 | 0.031 | **0.103** |
| same, controlling log(bars) and log(years) | **−0.376** | 0.002 | **0.017** |

The first row is the number to *not* quote on its own. Rows sharing an
instrument share a price series, so permuting rows overstates significance;
permuting whole instrument blocks is the honest test, and it says the raw
correlation is not significant.

Controlling for data quantity makes the effect *stronger*, not weaker — the
opposite of the usual worry. Sample size is strongly tied to the ratio
(ρ = −0.53: more bars, better resolution) and mildly tied to excess Sharpe
(ρ = −0.10: bar-rich series are the fast timeframes, which bleed fees), so both
paths push the same way and data quantity acts as a **suppressor**, masking the
real relationship rather than manufacturing it.

Decomposing the correlation shows what the estimator actually knows:

| dimension | pooled Spearman | reading |
|---|---|---|
| within a timeframe (rank instruments) | **−0.317** (p = 0.009) | real, and negative in **7 of 7** timeframes: 5m −0.43, 15m −0.33, 30m −0.21, 1h −0.51, 2h −0.26, 4h −0.33, 1d −0.07 |
| within an instrument (rank timeframes) | **+0.000** | nothing; signs scatter −0.57 … +0.83 |

**The gate ranks instruments, not timeframes.** It survives dropping any single
instrument (leave-one-out keeps ρ in [−0.368, −0.191], all negative). Remaining
caveats: this is still an in-sample ranking — the shortlist was selected on
full-sample spectra, so a walk-forward test of the gate itself is the
outstanding next step — and most (instrument, timeframe) pairs still lose to
buy-and-hold after fees. The gate concentrates the exceptions; it does not
manufacture them.

## Better instruments, measured: two that did not help

Two estimators were added and A/B'd properly. Both are more accurate than what
they replace. **Neither improves trading**, and that turned out to be the more
useful finding.

**Structure-function Hurst** (`--hurst-estimator sf|rs`, `indicators::hurstStructureFunction`).
Measured on fractional Brownian motion with known H at the rolling windows the
filter actually uses, it beats rescaled-range where it matters — at H=0.7 its
bias is −0.026 against R/S's −0.075, its spread is ~25% tighter, and its
false-positive rate on a true random walk at window 200 is 21.8% vs 29.6%.

Yet it made results worse (BTC `tsmom` Sharpe 1.31 → 0.81), a threshold sweep
was non-monotonic noise (AMZN: 0.45 → 0.34 → 0.55 → 0.13 → 0.08 across a 0.10
range, on 2–9 trades), and a held-out split killed the whole idea:

| | train 2016-21 (none/rs/sf) | held out 2022-26 (none/rs/sf) |
|---|---|---|
| BTC_USDT | 1.85 / **2.02** / 1.24 | 0.16 / **0.26** / 0.18 |
| AAPL | **0.99** / 0.74 / 0.71 | **0.14** / −0.08 / 0.04 |
| AMZN | **0.91** / 0.20 / 0.86 | 0.27 / **0.58** / 0.06 |

AMZN's R/S config goes from worst in training to best held-out. Out of sample
it is 3–3 between "no filter" and R/S, and the structure function never wins.
**The Hurst filter has no demonstrated edge with either estimator** — the
earlier "1.16 → 1.31 on BTC" was in-sample noise (held out: 0.16 → 0.26).

**HAR volatility forecasting** (`--vol-model trailing|har|pencil-har`,
`indicators/vol_forecast.h`). On a GARCH(1,1) series where the true conditional
variance is known, HAR cuts forecast MSE by **46%** and QLIKE by 48% against
the trailing window the sizer used before. It is strictly causal — gated by a
test asserting that recomputing from a truncated series reproduces every value
*bit-for-bit* (165 checks, worst difference exactly 0.0).

And it still sizes worse. At matched average exposure, `tsmom --vol-target`:

| Symbol | trailing Sharpe | HAR Sharpe (exposure-matched) |
|---|---|---|
| BTC_USDT | **1.26** | 1.17 |
| ASML.AS | **0.92** | 0.86 |
| AAPL | **0.60** | 0.43 |
| TSLA | **0.75** | 0.67 |
| META | **0.53** | 0.30 |
| AMZN | **0.53** | 0.34 |

### The horizon experiment, and why it also failed

The obvious diagnosis was a **horizon mismatch**: HAR forecasts one bar ahead,
but `tsmom` holds a position for 141 bars on BTC 4h and `odiseo` for 68. And
next-bar is the *least* predictable horizon there is — a trailing-30 estimate
explains R² = 0.13 of next-bar volatility but 0.39 of volatility over the next
141 bars. `order-spectrum` independently justified the fix, measuring BTC's
volatility memory as fractional with d ≈ 0.38 (an exponential-memory control
matched at h=1 retains only R² = 0.067 at h=141, against the real 0.390).

So `--vol-horizon` was added: forecast the *average* volatility over the next H
bars, plus `--vol-model fractional`, whose regressors use power-law memory
weights j^(d−1) with d estimated causally.

**The forecasting hypothesis was confirmed.** HAR's edge over trailing grows
roughly eightfold with horizon (QLIKE improvement, BTC −7.4% → **−57.8%**; AAPL
−2.8% → −59.1%; GARCH with known truth −48.2% → −78.4%), and the causally
estimated d on BTC came out **0.380** — matching `order-spectrum`'s 0.38 from a
completely independent estimator.

**The trading hypothesis was falsified.** Sharpe falls monotonically as the
horizon is matched:

| vol-model | H=1 | H=20 | H=68 | H=141 | H=300 |
|---|---|---|---|---|---|
| trailing | **1.26** | 1.26 | 1.26 | 1.26 | 1.26 |
| har | 1.15 | 1.15 | 1.13 | 1.11 | 1.10 |
| fractional | 1.17 | 1.16 | 1.13 | 1.10 | 1.11 |

and horizon-matched HAR loses to trailing on **all six** instruments, raising
drawdown on three of them.

The mechanism is visible in the exposure: going from trailing to HAR at H=141
cuts average position 49.4% → 33.4% (×0.68) but max drawdown only 39.7% →
31.7% (×0.80). **The exposure it removes is less risky than average** — a
better volatility forecast cuts position more decisively in high-volatility
regimes, and for a trend follower those are precisely where the payoff is
concentrated. Volatility targeting trades return for risk; a more accurate
forecast makes that trade more aggressively, and here it trades away more than
it buys.

So the finding is not "the forecast was bad" — it was measurably, independently
better. It is that **the sizing rule, not the volatility estimate, is what
limits this.** Three separate attempts at a better instrument (structure-function
Hurst, HAR, horizon-matched HAR) each improved the measurement and each traded
worse. That is now the most reliable regularity in this repo, and it says the
next thing to change is the objective, not the estimator.

Defaults are therefore unchanged (`trailing`, `H=1`), every alternative ships
behind a flag, and the honest status of each is documented at its definition.

## Data integrity

- **Forming candles are never stored.** Poloniex pages backward from *now*, so
  the in-progress candle was always included and — because the store is
  append-only and later fetches filter `timestamp <= since` — its partial
  OHLCV was frozen permanently. The shipped BTC file's last bar was captured 66
  minutes into its 4-hour window, at ~27% of neighbouring volume.
- **Re-based history is detected.** `fetch` deliberately re-fetches a few stored
  bars and compares them. Yahoo re-scales its whole history across a stock
  split, so an append-only store would otherwise glue two price bases together
  with a fabricated one-day crash. `--repair` rewrites the store atomically.
- **Yahoo prices are split- *and* dividend-adjusted** by default (O/H/L/C scaled
  by `adjclose/close`), so the series is total return and free of ex-dividend
  gaps that strategies read as selling pressure. `--raw-prices` opts out.
- **Everything is validated on append**: monotonic timestamps, OHLC invariants,
  non-finite values, outliers, and spacing that isn't an integer multiple of the
  period — while correctly tolerating weekends, holidays and DST shifts.
- **Writes are crash-safe.** `append` refuses a store whose data region isn't a
  whole number of records and writes durably, so an interrupted append cannot
  leave a misaligned file whose next append would cement garbage into history.
  `--repair` validates the replacement before writing and renames atomically.

`cli_trader validate` reports on any store. The six shipped files are clean
apart from 498 genuinely zero-volume 4h BTC bars from Poloniex's thin early
history, which are reported as informational rather than treated as corruption.

## Architecture

```
src/
  core/        Candle + CandleSeries (SoA) + binary CandleStore + validation
  data/        HttpClient (libcurl) + Poloniex/Yahoo sources (closed bars only)
  concurrency/ ThreadPool + RateLimiter
  indicators/  SMA, EMA, RSI, ATR (Wilder), DMI/ADX, Ichimoku, Bollinger, Hurst
  strategy/    Strategy interface (pure fn of series+bar+PositionContext),
               Odiseo, PureIchimoku, SmaCross, Tsmom, Emergent, HurstRegimeFilter
  backtest/    BacktestEngine + metrics.h (annualized Sharpe, deflated Sharpe,
               PSR, Sortino, intrabar drawdown)
  evolution/   fitness.h (excess-Sharpe minimax) + GeneticOptimizer +
               StrategyEvolver
  trading/     TradingClient (paper/Poloniex), LiveTrader (replay + idempotent
               orders + lockfile), TradingState (atomic), TradeLog
  main.cpp     fetch / validate / backtest / list-strategies / run / status /
               parity / evolve / evolve-strategy
```

Adding a strategy = implement `Strategy` (`prepare` + `onBar(series, i, position)`)
and register it in `main.cpp`'s `makeStrategy()`. Adding a data source = match
the shape of `PoloniexSource`.

## What would actually make this profitable

Short version: not more
evolution on these six correlated instruments. The remaining levers are
**breadth** (many uncorrelated markets with risk aggregated at the portfolio
level — vol targeting already frees half the capital, and the best configuration
sits flat 67% of the time), a **venue decision**, and shorting. The realistic
ceiling for this design is a vol-targeted multi-market trend follower at Sharpe
~0.8-1.2, which is a respectable business and nothing like the four- and
five-figure headline returns a long/flat rule shows on a decade-long bull
market.

The number to watch is **out-of-sample excess Sharpe over buy-and-hold, sized by
the worst walk-forward segment rather than the mean**.
