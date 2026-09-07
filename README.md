# cli_trader

A backtesting-focused C++17 CLI for systematic trading research. It fetches
OHLCV history from Poloniex and Yahoo, validates the stores, backtests any of
~40 registered strategy families against a fee-matched buy-and-hold benchmark,
searches parameter and rule space, and can run any of those strategies paper or
live against a real venue.

It is a toolkit, not a strategy. Nothing here is investment advice, no
configuration in this repository is a recommendation, and the research notes it
links to are mostly records of things that did **not** work.

## Build

Requires CMake ≥ 3.16, a C++17 compiler, libcurl and OpenSSL (system-provided on
macOS/Linux). `nlohmann/json` is fetched automatically via CMake `FetchContent`.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Quick start

```sh
# Pull public OHLCV history. Never stores the candle that is still forming, and
# re-checks a few already-stored bars against the venue before appending.
./build/cli_trader fetch --symbol BTC_USDT --period 14400 --data-dir data

# Check a store's integrity (spacing, OHLC invariants, outliers, gaps).
./build/cli_trader validate --symbol BTC_USDT --period 14400

# See every strategy family with its searchable parameter ranges.
./build/cli_trader list-strategies

# Backtest one. Every report includes a buy-and-hold benchmark charged the
# same fees, and an excess column - the only one that says anything.
./build/cli_trader backtest --symbol BTC_USDT --period 14400 --strategy tsmom \
    --vol-target 0.20
```

Supported `--period` values (seconds): `300, 900, 1800, 7200, 14400, 86400` for
Poloniex; `300, 900, 1800, 3600, 86400, 604800` for Yahoo.

## Commands

`cli_trader --help` prints every flag. One line each:

| command | what it does |
|---|---|
| `fetch` | append closed candles from Poloniex or Yahoo; detects re-based history (stock splits) and repairs it atomically |
| `validate` | report spacing, OHLC invariants, outliers and gaps in a store |
| `list-strategies` | every registered family, its provenance, and its searchable parameter ranges |
| `backtest` | one strategy on one instrument, against a fee-matched buy-and-hold benchmark |
| `portfolio` | several instruments as one book, against an equal-weight basket of the same instruments |
| `xsmom` | cross-sectional momentum: rank the universe, hold the leaders, both sides fully invested |
| `order-spectrum` | estimate the *order* of a series' memory; see [docs/ORDER_SPECTRUM.md](docs/ORDER_SPECTRUM.md) |
| `evolve` | genetic search over one strategy's parameters, with anchored walk-forward validation |
| `evolve-strategy` | genetic programming over the rules themselves, from a scale-invariant feature set |
| `tournament` | breed whole strategy *families* against each other, with controls; see [docs/TOURNAMENT.md](docs/TOURNAMENT.md) |
| `run` | live or paper trading loop: replay, one order per cycle, atomic state |
| `status` | what a running sleeve believes about its position |
| `parity` | do the live fills match what the engine would have done? |
| `balances` | read real account holdings (read-only; needs API keys) |
| `order` | **places a real order.** Manual tool, no strategy involved; `--aggression` sets how far through the book it crosses |

## Flags worth knowing

`--strategy` picks a family and `--sparams "name=value,..."` sets its
parameters, using only the names and ranges `list-strategies` prints for that
family. An unknown name is an error rather than a silent no-op.

```sh
./build/cli_trader backtest --symbol ETH_USDT --period 14400 \
    --strategy donchian --sparams "entryWindow=55,exitWindow=20,atrStopMult=2.5" \
    --vol-target 0.20 --start 2018-01-01 --end 2023-12-31
```

`--vol-target` sizes each entry for a target annualized account volatility
instead of going all-in; `--vol-window` sets the estimation window.
`--fill-timing` chooses next-open (the honest default) or same-close.
`--fee` and `--slippage` are charged on both sides of every trade.
`--start` / `--end` restrict the window, and `--warmup-bars` feeds indicators
from before `--start` without trading or scoring those bars.

## Strategy families

`list-strategies` is the authority. The registry spans classical trend
following (moving-average, breakout, time-series momentum, Ichimoku, adaptive
averages), mean reversion, volatility-regime rules, several published academic
families, ensembles that vote across members, a declarative rule language
(`generated_spec`) that composes new rules from causal primitives, and two
**controls** — coin-flip entries and buy-and-hold — that are searched exactly
like the rest so a result can be compared against what selection pressure
extracts from something that cannot work.

Every family publishes its parameters as named, bounded scalars, which is what
lets one search implementation cover all of them. A worked example
configuration is described in plain English in [docs/STRATEGY.md](docs/STRATEGY.md).

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

## Live and paper trading (`run` / `status` / `parity`)

`run` polls for freshly **closed** candles, replays every bar the strategy has
not seen yet, and places at most one order per cycle to move the account to the
position the strategy wants. Paper mode needs no API keys and is charged the
same fee and slippage the backtest charges.

```sh
./build/cli_trader run --symbol BTC_USDT --period 14400 --strategy ensemble_vote \
    --vol-target 0.30 --mode paper --poll-interval 60 --data-dir data --state-dir state

./build/cli_trader status --symbol BTC_USDT --state-dir state
./build/cli_trader parity --symbol BTC_USDT --period 14400 --strategy ensemble_vote
```

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
paper P&L, is the evidence that backtest results transfer.** A book that
cold-started mid-history will show a low rate until every sleeve has been
through one signal-driven round trip; that is a property of the book, not a
defect in the strategy.

Real orders (`--mode live`) require `POLONIEX_API_KEY` and
`POLONIEX_API_SECRET`. The Poloniex client is written against the current spot
API and verified against its documented examples and RFC 4231 vectors, but it
is **not verified against a real account by this repository**; it carries a
pre-flight checklist in its header. Keys are never hardcoded.

Operating a deployment, including the failure modes worth monitoring, is in
[docs/RUNBOOK.md](docs/RUNBOOK.md).

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

**Numbers move as stores grow.** Every figure in these docs was measured on the
candle stores as they stood at a particular moment, and `fetch` appends. A fresh
run will not match to the last digit; pin `--end` to reproduce one exactly. When
a control number moves, check the bar count before you check the code.

## What the research found

Short version, with the evidence behind each link:

- **Buy-and-hold usually wins.** Across four symbols and four strategies, 15 of
  16 combinations lose to it on risk-adjusted return. [docs/RESULTS.md](docs/RESULTS.md)
- **Volatility targeting is the one result that reproduces.** It is a leverage
  dial: return and drawdown scale together while Sharpe stays flat. [docs/RESULTS.md](docs/RESULTS.md)
- **Portfolio construction beats signal selection.** Combining imperfectly
  correlated sleeves is arithmetic, not forecasting. [docs/RESULTS.md](docs/RESULTS.md)
- **Large searches mostly find luck.** ~53,000 genomes across 24 families; among
  plausible candidates, in-sample rank carries no out-of-sample information, and
  coin flips ranked first in 4 of 12 runs. [docs/TOURNAMENT.md](docs/TOURNAMENT.md)
- **Better instruments made worse strategies.** Three separate cases where a
  measurably better estimator traded worse. [docs/NEGATIVE_RESULTS.md](docs/NEGATIVE_RESULTS.md)
- **The old evolutionary results were meaningless**, and the reason is
  instructive. [docs/EVOLUTION.md](docs/EVOLUTION.md)
- **How a hypothesis gets registered, run once, and killed.** [docs/RESEARCH_PROTOCOL.md](docs/RESEARCH_PROTOCOL.md)

## Limitations

Stated plainly, because they bound everything above.

- No strategy in this repository has cleared walk-forward validation.
- The Poloniex trading client is unverified against a real account.
- Backtests model fees, slippage and next-open fills, but not order-book depth,
  partial fills at scale, venue outages, or funding on leveraged products.
- Below 4h bars, round-trip costs dominate every effect measured here.
- The research record is a record of one person's search. Treat it as a lab
  notebook, not a result.

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
  main.cpp     fetch / validate / backtest / portfolio / xsmom /
               list-strategies / order-spectrum / evolve / evolve-strategy /
               tournament / run / status / parity / balances / order
```

Adding a strategy = implement `Strategy` (`prepare` + `onBar(series, i, position)`;
a strategy that needs a second instrument reads it through `zoo::MarketContext`
and `zoo::alignByTimestamp` in `strategy/zoo/market_context.h`, never by index)
and register it in `main.cpp`'s `makeStrategy()`. Adding a data source = match
the shape of `PoloniexSource`.

## Documentation

| file | what is in it |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | layers, processes, threads, state machines, with diagrams |
| [docs/STRATEGY.md](docs/STRATEGY.md) | a worked configuration in plain English, and why it looks like that |
| [docs/RUNBOOK.md](docs/RUNBOOK.md) | operating a deployment, and what normal looks like |
| [docs/RESULTS.md](docs/RESULTS.md) | measured results and how the benchmarks are built |
| [docs/NEGATIVE_RESULTS.md](docs/NEGATIVE_RESULTS.md) | what was tried and did not work |
| [docs/RESEARCH_PROTOCOL.md](docs/RESEARCH_PROTOCOL.md) | hypothesis registration and kill rules |
| [docs/TOURNAMENT.md](docs/TOURNAMENT.md) | the family-level strategy search |
| [docs/EVOLUTION.md](docs/EVOLUTION.md) | parameter and rule evolution |
| [docs/ORDER_SPECTRUM.md](docs/ORDER_SPECTRUM.md) | the memory-order estimator |
| [docs/INDICATORS.md](docs/INDICATORS.md) | indicator semantics and past corrections |
| [docs/ENSEMBLE_INVOL.md](docs/ENSEMBLE_INVOL.md) | inverse-volatility sleeve weighting (research variant) |

## License

Copyright (c) 2026 Ulises Merlan

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
