# Factor trend: reading an altcoin's trend through Bitcoin's

**Status: measured, built, pre-registered, run once, NOT adopted.** The
signal-level effect is real; it does not survive portfolio construction, and
the reason is measured rather than guessed. Everything below used development
data only (before 2024-01-01). `factor_trend` at any blend weight has never
been run on the 2024+ window.

## 1. The question

Every strategy family in this repo up to 2026-09-05 read one price series.
Crypto is close to a one-factor market: an altcoin's return is roughly beta
times the market's plus a large idiosyncratic term, with beta above one. So an
alt's own 15-day return is a *noisy* estimate of the persistent, market-wide
move that trend following actually feeds on, and Bitcoin - the least noisy
proxy for that factor - is a second, partly independent reading of the same
quantity. In equities, momentum in individual stocks is largely *factor*
momentum (Ehsani & Linnainmaa, "Factor Momentum and the Momentum Factor",
Journal of Finance 2022). The hypothesis: blend each coin's trend statistic
with Bitcoin's and the false signals on alts drop out.

This was the one source of *new information* available in `data/` for a 4h
long/flat book: not another slicing of the same candles, but other candles.

## 2. The measurement, before anything was built

`measure_factor_conditioning.py` is pre-registered in its own docstring:
tsmom's statistic `z = (close[t]/close[t-90] - 1) / (sigma_30 * sqrt(90))`
on the coin and on BTC (read one bar late, see §3), the *tradable* next-bar
return `open[t+1] -> open[t+2]`, pooled over the 7 alts, development data.

Full-history output is in `measurement_full_history.txt`; **do not read it**
without the caveat below. The table of record is `measurement_2017plus.txt`
(`FT_START=1483228800`, i.e. 2017-01-01):

| state (2017-2023, 7 alts pooled) | bars | share | ann. return | Sharpe |
|---|---:|---:|---:|---:|
| own z > 0, BTC z > 0 | 30,651 | 40.8% | +164% | **1.45** |
| own z > 0, BTC z < 0 | 6,789 | 9.0% | +97% | 0.69 |
| own z < 0, BTC z > 0 | 11,204 | 14.9% | −34% | −0.40 |
| own z < 0, BTC z < 0 | 26,433 | 35.2% | −68% | −0.64 |
| own z > 0, any BTC | 37,440 | 49.9% | +152% | 1.28 |
| BTC z > 0, any own | 41,855 | 55.7% | +111% | 1.05 |

Read: Bitcoin's trend carries information *given* the coin's own trend - the
"confirmed" state earns twice the Sharpe of the "unconfirmed" one - but it is
not a substitute (own alone 1.28 beats BTC alone 1.05) and it does not rescue
a coin whose own trend is down (−0.40). The blend's dose-response, holding
while `(1-w)·own + w·BTC > 0.5`:

| w | 0 | 0.25 | 0.5 | 0.75 | 1 |
|---|---:|---:|---:|---:|---:|
| pooled Sharpe (pre-fee, bar level) | 1.48 | 1.51 | **1.64** | 1.50 | 1.18 |

A hump, not a spike; 5 of 7 coins improve at w = 0.5. A leave-one-out basket
of the other coins as the factor was tried alongside and was *weaker* than
Bitcoin alone in the blend (1.53 peak), so BTC was kept as the reference.

**Data-quality caveat that changed the picture.** Run over the full history
(from 2015) the same table shows a far larger effect: the unconfirmed state at
Sharpe −0.07 and the "rescue" state at +0.21. That is an artifact. XRP and LTC
against USDT were nearly untraded on Poloniex in 2015-16; long runs of stale,
carried-forward bars make the rolling volatility tiny and the z-scores
explode, and the Pearson correlation of own vs BTC z came out at **−0.003**
(XRP) and **−0.013** (LTC) - impossible for coins that co-move at 0.7. From
2017 the correlations read 0.54 and 0.71 and the effect shrinks to the table
above. The protocol folds start in 2018, so the 2017+ table is the relevant
one, and it was the one used to fix the design. The lesson is worth its own
line: *a conditional-return table on thin early history can manufacture an
effect several times the real one.*

## 3. The strategy: `factor_trend`

`src/strategy/zoo/factor_trend.h`, registered in `registry.h`.

```
blend[i] = (1 - w) * z(coin)[i] + w * z(BTC)[latest bar <= t_i - refLagBars * period]
Buy while blend > +thresholdZ, Sell while blend < -thresholdZ (tsmom's hysteresis)
```

Parameters and their defaults are tsmom's live defaults (`lookback=90`,
`volWindow=30`, `thresholdZ=0.5` sigma) plus the one new degree of freedom,
`factorWeight` (w). Three properties are enforced by tests
(`tests/factor_trend_test.cpp`, in CTest) and by the causality tool:

- **Regression anchor.** At w = 0 the family *is* tsmom with a sigma threshold
  and no volatility gate. On ETH 4h 2018-2023 both print ending equity
  3259.04, 59 trades, Sharpe 0.90 - to the cent.
- **Causal, cross-series.** The reference store on disk is the full history
  whatever slice a fold hands to `prepare()`, so reference bars are aligned by
  **timestamp**, never by index (`zoo::alignByTimestamp` in
  `market_context.h`). `causality_check` passes on both the ETH store (where
  the family reads BTC) and the BTC store.
- **Live parity by construction.** The reference is read one bar late. Live,
  each sleeve is its own process; when the ETH sleeve evaluates the bar that
  closed at T, the BTC process may not have fetched BTC's bar T yet. Reading
  T − 1 needs the BTC store to be at most one bar stale, which the 60-120 s
  poll loop guarantees, and the backtest reads the same lagged bar. For a
  15-day statistic, four hours of lag is immaterial. `MarketContext` re-reads
  a reference store whose last timestamp has advanced (the live loop calls
  `prepare()` every cycle), and `prepare()` warns on stderr if the reference
  ends more than a bar behind what the newest decision needs.
- **No silent fallback.** A missing reference store is a hard error. Degrading
  to own-only would produce a report labelled `factor_trend` that measured
  tsmom.

The infrastructure is the reusable part: `zoo::MarketContext` is set once
from `--data-dir` in `main()`, so `backtest`, `portfolio`, `tournament`, `run`
and `parity` all resolve a second instrument from the same place the traded
series came from. Any future family that needs another market (breadth, a
macro proxy, a funding series aligned to candles) gets it through the same
two calls.

## 4. The pre-registered test

Registered as `factor-trend-w050-vt020` in `hypotheses.json` **before** any
engine-level number was produced, with `factorWeight=0.5` fixed by prior (the
agnostic equal weight; it was chosen against the full-history table's apparent
optimum at 0.75, and before the 2017+ table was seen). Run once under
`research_protocol.py`: 4-env universe, three chronological folds, 80-bar
causal warm-up, vt 0.20, equal weights.

**Result: `survives_development`** - mean excess Sharpe over the basket +0.25,
worst fold +0.20, 3/3 folds positive, worst drawdown 24.3%. That label means
only that it cleared the basket-relative kill rules. The question that matters
is whether it beats what the book already runs, on the same folds
(`dev_sweep.py`, `results.json`):

| configuration | 2018-19 | 2020-21 | 2022-23 | mean excess | worst |
|---|---|---|---|---:|---:|
| **factor_trend w=0.5 (registered)** | −0.05 / +0.31 | 1.97 / +0.20 | 0.21 / +0.23 | **+0.25** | +0.20 |
| anchor w=0 (tsmom, 0.5σ, no gate) | 0.05 / +0.41 | 2.02 / +0.24 | 0.45 / +0.48 | +0.38 | +0.24 |
| tsmom, live defaults | 0.34 / +0.70 | 2.28 / +0.50 | 0.25 / +0.27 | +0.49 | +0.27 |
| ensemble_vote 2/0 (live rule) | 0.14 / +0.50 | 2.19 / +0.42 | 0.30 / +0.32 | +0.41 | +0.32 |

(cells: portfolio Sharpe / excess Sharpe vs the EW basket)

The candidate trails its own anchor on all three folds and both incumbents on
mean and worst excess. **It loses.**

## 5. Why: the decomposition

Portfolio Sharpe is mean sleeve Sharpe times the diversification multiplier
`sqrt(N / (1 + (N−1)ρ))`. The dose-response at the engine level, full dev
window (2018-01-01..2023-12-31, 4 sleeves, vt 0.20):

| w | portfolio Sharpe | mean sleeve Sharpe | sleeve correlation ρ | trades |
|---|---:|---:|---:|---:|
| 0 | **0.98** | 0.78 | 0.514 | 234 |
| 0.25 | 0.86 | 0.70 | 0.550 | 224 |
| 0.5 | 0.81 | 0.67 | 0.573 | 225 |
| 0.75 | 0.80 | 0.68 | 0.617 | 234 |
| 1 | 0.90 | 0.77 | 0.646 | 240 |

Two things go wrong at once, and they are separable in this table:

1. **The bar-level edge does not survive being a position rule.** Mean sleeve
   Sharpe *falls* from 0.78 to 0.67 at w = 0.5, where the bar-conditional
   table promised a gain. The blend's hysteresis holds an alt through its own
   trend break while Bitcoin's is intact - exactly the (own<0, BTC>0) state
   that the 2017+ table shows earning −0.40 - and the exit side gives back
   what the entry side gained. Per sleeve: ETH improves (0.90 → 1.03), XRP and
   LTC lose (0.56 → 0.33, 0.48 → 0.34).
2. **Shared information synchronizes the sleeves.** ρ rises monotonically with
   w, 0.51 → 0.65. Sleeves that read the same factor enter and exit together,
   so the multiplier the book lives on shrinks from 1.26 to 1.18. This was
   predicted in the strategy's header before the run, and it is the same
   arithmetic as addenda 19 and 21: *effective breadth is the binding
   constraint on this book in both directions*, and a common signal spends it.

Note that the diversification prediction is exact in every row
(`mean × multiplier = achieved` to two decimals), as it has been every time
this repo has checked it.

## 6. The one variant the table supports - also negative (second look)

The 2017+ table says the factor's information is in the *veto*, not the
rescue: given own > 0, Bitcoin down halves the Sharpe; given own < 0, Bitcoin
up does not help. The symmetric blend spends half its weight on the half that
fails. So a `confirmZ` knob was added *after* the engine result was seen - a
second look at the development data, recorded as such: a Buy additionally
requires BTC's z above g; Sells are untouched, so Bitcoin can keep a coin out
but never keep it in.

| g (w = 0) | 4-env dev Sharpe | 8-coin 2021-23 Sharpe |
|---|---:|---:|
| off (anchor) | **0.98** | 0.87 |
| −0.5 | 0.97 | **0.90** |
| 0 | 0.93 | 0.87 |
| +0.5 | 0.87 | 0.75 |

Monotonically worse as the confirmation tightens on the research universe,
flat-then-worse on the deployment one. The bars it removes (own up, BTC down)
still earned Sharpe 0.69; removing positive bars lowers exposure and return
and buys nothing in drawdown (22.6% vs 23.8%). This is the twelfth
exposure-reducing filter to fail here, and it fails for the usual reason.

## 7. The deployment universe (descriptive)

8 coins, 2021-01-01..2023-12-31, live sizing (vt 0.30, vol-window 90), the
real 0.125% fee and 0.05% slippage:

| configuration | Sharpe | max DD | sleeve ρ |
|---|---:|---:|---:|
| LIVE ensemble_vote 2/0 | **0.96** | 19.9% | 0.384 |
| anchor w=0 | 0.87 | 17.8% | 0.394 |
| factor_trend w=0.5 | 0.74 | 21.1% | 0.443 |
| factor_trend w=1 | 0.53 | 29.8% | 0.516 |

Same ordering, same mechanism, higher drawdown as well. No risk-matched
de-levering control is needed: the candidate is dominated on Sharpe *and*
drawdown at every w, so there is nothing to match.

## 8. Verdict

**Not adopted.** Bitcoin's trend does carry information about an altcoin's
next bar beyond the altcoin's own trend - that measurement stands, from 2017
on, at about a factor of two in conditional Sharpe. It cannot be converted
into portfolio return by any of the four ways a long/flat rule can use a
second series: blending (raises sleeve correlation, and the exit side gives
the entry side back), confirming entries (removes bars that were still
positive), holding through own-trend breaks (the −0.40 state), or exiting on
Bitcoin's break (the +0.69 state). The first two were run; the last two are
excluded by the conditional table itself.

What the study adds to the book's understanding: the binding constraint is
still effective breadth, and it binds against *information sharing* the same
way it binds against sizing, reallocation and adding coins. A signal that is
common to the sleeves is, at the portfolio level, partly a bet on the factor,
and the book already holds that bet through beta.

What it adds to the code: a tested, causal, live-safe way for a strategy to
read a second instrument, and a family that uses it whose w = 0 is a
bit-exact tsmom, so anyone can re-run every table here with one flag.

## 9. Protocol notes

- Looks at the development data by `factor_trend`: the registered run (one),
  the dose-response and per-sleeve decomposition (the same window, several
  weights, disclosed above as exploratory), the confirmation knob (a second
  look, disclosed). Nothing here is presented as fresh evidence for a variant
  chosen after the fact.
- **The 2024+ window was never used by `factor_trend`.** A single
  pre-registered look would be permitted by `holdout.json`; it is **not
  recommended** - the candidate already loses to the incumbent on development
  data, and addendum 6's rule applies: do not spend a look confirming the
  death of something with no life left.
- **A slip, self-reported.** One ad-hoc shell loop comparing the *incumbents*
  (tsmom, ensemble_vote, the w = 0 anchor) on the three folds relied on zsh
  word-splitting a `"start end"` pair, which zsh does not do; the loop ran
  each "fold" from its start date to the end of the stores, i.e. into 2024+.
  Those three families are already recorded as contaminated on 2024+
  (`holdout.json`), so no clean data was spent, and the numbers were
  discarded and regenerated by `dev_sweep.py`, which passes arguments as a
  list. Addendum 22 warned about exactly this; it is repeated here because it
  bit again.

## 10. Files and reproduction

```
experiments/factor_trend/
  measure_factor_conditioning.py   pre-registered signal-level measurement (pure Python)
  measurement_2017plus.txt          table of record  (FT_START=1483228800)
  measurement_full_history.txt      full history, see the data-quality caveat in §2
  dev_sweep.py                      every engine-level table above, dev data only
  results.json                      its output, with the exact command per row
src/strategy/zoo/market_context.h   cross-series references + timestamp alignment
src/strategy/zoo/factor_trend.h     the family
tests/factor_trend_test.cpp         anchor, alignment, causality, confirmation, missing-reference
experiments/hypotheses.json         factor-trend-w050-vt020 (survives_development; see §4)
```

```sh
cmake --build build -j && ctest --test-dir build
./build/experiments/tools/causality_check data/ETH_USDT_14400.ctc
FT_START=1483228800 python3 experiments/factor_trend/measure_factor_conditioning.py
python3 experiments/factor_trend/dev_sweep.py
./build/cli_trader backtest --symbol ETH_USDT --period 14400 --strategy factor_trend \
    --sparams factorWeight=0.5 --vol-target 0.20 --start 2018-01-01 --end 2023-12-31
```
