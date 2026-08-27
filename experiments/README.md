# The (instrument, timeframe) order gate

Does the logarithmic-spiral order estimator identify **where** trend following
works?

The estimator (`src/math/spiral.h`, `src/math/pencil.h`, exposed as
`cli_trader order-spectrum`) measures the *order* of a series' volatility
memory. The claim under test is that it can be used as a **gate**: select the
(instrument, timeframe) pairs whose volatility memory is *resolvably
fractional*, trade the `tsmom` trend strategy only there, and leave the rest
alone.

> **Read this first.**
>
> 1. The headline result is real and reproduces exactly today, but only through
>    `im_rescore.py`. **`gate_sweep.py report` is currently broken** — a fresh
>    `gate_sweep.py run` produces `+0.033` instead of `-0.233`, because the
>    `order-spectrum` output gained a column and the parser was never updated.
>    See [Reproducibility status](#reproducibility-status). The recorded JSONs
>    are unaffected.
> 2. The shortlist below was selected on **full-sample** spectra. It is **not**
>    an out-of-sample result and nothing here has cleared walk-forward
>    validation. Do not trade it.
> 3. `experiments/fetch/` does not exist. The script that produced the equity
>    intraday stores was never ported, so those stores **cannot be regenerated
>    from this repo**. See [Data](#2-the-data).

---

## 1. The question

`order-spectrum` evaluates the truncated Laplace transform of a series'
autocorrelation on a logarithmic spiral `s = r·e^{(σ+i)u}` and reads the order
off a matrix-pencil line spectrum. A spiral rather than a circle because on the
unit circle the transform of time-domain data is exactly 2π-periodic, so its
inverse is supported on the integers and a non-integer order can never appear.
Any σ ≠ 0 breaks that.

Applied to a decaying autocorrelation, the recovered α says which model class
the memory belongs to:

| α | meaning |
|---|---|
| ≈ −1 | integer order: exponential relaxation, ordinary ARMA memory |
| −1 < α < 0 | fractional order: power-law long memory, ACF ~ k^−(α+1) |

**The value is not the statistic; the convergence is.** A genuine fractional
order is radius-independent once the contour reaches the band where the power
law lives. So the experiment's gate variable is not α but how *stable* α is as
the contour shrinks, normalised by how stable a **known** fractional series
looks under the same lag budget:

```
        tail spread of α over the 3 smallest radii
ratio = ------------------------------------------------------------
        max tail spread of the two power-law references (k^-0.3, k^-0.6)
```

Low ratio = the order is resolvable = the series has memory the estimator can
pin down. The pre-registered prediction is therefore a **negative** rank
correlation between `ratio` and `tsmom` **excess Sharpe** (strategy Sharpe
minus buy-and-hold Sharpe) across (instrument, timeframe) stores.

Excess Sharpe, not return. A long/flat rule on assets that rose 15x–280x always
shows a big total return; the excess column is the only one that says anything
about the strategy.

---

## 2. The data

98 (instrument, timeframe) stores across two venues.

### Venues and depth

**Crypto — Poloniex**, 8 USDT pairs, fetched to full venue depth.

| Pair | 15m bars | span | first bar |
|---|---:|---:|---|
| BTC_USDT | 403,181 | 11.50 y | 2015-02-19 |
| XRP_USDT | 403,080 | 11.50 y | 2015-02-20 |
| LTC_USDT | 401,633 | 11.45 y | 2015-03-07 |
| ETH_USDT | 372,491 | 10.62 y | 2016-01-05 |
| DOGE_USDT | 279,443 | 7.97 y | 2018-08-31 |
| TRX_USDT | 237,398 | 6.77 y | 2019-11-12 |
| SOL_USDT | 166,257 | 4.74 y | 2021-11-22 |
| ADA_USDT | 161,972 | 4.62 y | 2022-01-06 |

Getting that depth required **fixing a pagination-truncation bug** in
`src/data/poloniex_source.cpp`. Poloniex's public candles endpoint does not
page *forward* via `startTime` (it returns the most recent `limit` candles
regardless), so the fetcher walks *backward* via `endTime`. The bug was in the
termination test: a **short page** was treated as end-of-history. A short page
usually means a venue-side maintenance gap with years of history behind it, so
the walk stopped early — it truncated the 15m walk at 469 days where the venue
actually serves 8+ years, and XRP at 1.4 years against a true 11.5. The fix is
that only an **empty** page ends the walk, never a short one.

The venue also **refuses windows at random depths**, and neither one retry nor
several is enough. The fetcher escalates in two stages: retry the same window
with growing backoff, then **skip** back one page width and keep walking. A
skipped window loses at most `limit` bars and says so loudly; an absolute page
cap backs it up.

**Equity — Yahoo Finance**, 5 symbols (AAPL, AMZN, META, TSLA, ASML.AS).

| Rung | bars | span |
|---|---:|---:|
| 1d | 2,512–2,559 | 10.00 y |
| 1h | 5,076–6,554 | 2.86–2.91 y |
| 15m / 5m | 1,539–6,109 | 0.23 y |

Yahoo caps intraday history hard: the 5m and 15m rungs cover about **12 weeks**.

**Equity intraday is not dividend-adjusted.** The daily stores were produced by
`cli_trader fetch`, which recovers a per-bar adjustment factor as
`adjclose/close` and applies it to the whole bar, giving a total-return series.
Yahoo does not publish `adjclose` for intraday intervals, so every intraday
rung is raw. Consequence worth stating plainly: **within a single equity
ladder, the 1d rung is total-return adjusted and the 5m/15m/1h/2h/4h rungs are
not.** Comparing across timeframes for equities therefore mixes two price
definitions, and unadjusted dividend payers print a mechanical gap down on each
ex-div date that a trend strategy reads as selling pressure.

### Layout

| Path | Contents | Count |
|---|---|---:|
| `data/` | pulled venue stores, `<SYMBOL>_<PERIOD>.ctc` | 43 |
| `data/derived/` | regenerated resamples, same naming | 50 |
| `data/derived/fw/` | matched-window truncations, same naming | 5 |

Deriving a pair's 30m–4h rungs from its **one** deep 15m store makes every rung
of that ladder cover an identical window, so "timeframe" is not confounded with
"which years of market you looked at". `fw` ("fixed window") is the 10-year
equity daily series truncated to start at that symbol's 1h first bar, so the 1d
rung can be compared against the intraday ladder on the same window.

| Profile | Rung | Source |
|---|---|---|
| crypto | 5m, 15m | pulled |
| crypto | 30m, 1h, 2h, 4h | derived from that pair's 15m |
| crypto | 4h `~full` | pulled (deeper direct 4h fetch) |
| crypto | 1d `~full` | derived from the pulled 4h |
| equity | 5m, 15m, 1h | pulled |
| equity | 2h, 4h | derived from that symbol's 1h |
| equity | 1d | pulled (10-year daily) |
| equity | 1d `~fw` | daily truncated to the 1h window |

### Results-file provenance

`order_gate/results/gate_results.json` used to record the absolute `datadir`
each row was measured from — on one machine that was a session-scoped
temp directory, i.e. a path with no meaning anywhere else. Those fields now
hold the store **kind** (`<layout:pulled>`, `<layout:derived>`) and every row
carries `store_kind`; `im_rescore.resolve_store` never read the path anyway,
it resolves through `(profile, period, tag)`. The `derived` stores those
rows were measured on live in `data/derived/` (50 stores, ~107 MB, gitignored
with the rest of `data/`) plus `data/derived/fw/` for the walk-forward
windows; they are regenerated from the pulled 15m/4h stores by
`order_gate/build_stores.py`, so the sweep is fully re-runnable on any machine
that has run that script. The `.wf_cache*.json` memoization files were removed
from the repo (uncited caches; the walk-forward scripts rebuild them).

### Gaps and irregularities

- **`data/DOGE_USDT_300.ctc` is missing.** DOGE is the only crypto pair without
  a 5m store; its fetch never completed and venue throttling makes
  re-acquisition expensive. The manifest has 99 rows, 98 present. The crypto
  panel is asymmetric at 5m: 7 pairs at 300s, 8 at 900s/14400s.
- **The equity intraday stores come from `experiments/fetch/yahoo_intraday.py`**
  (added 2026-08-20; an earlier draft of this note said the fetcher lived only
  in a scratchpad — no longer true). It phase-filters Yahoo's response so
  off-grid stub bars are dropped and `CandleStore::append`'s spacing validation
  passes. `data/AAPL_300.ctc` and its 13 siblings can be regenerated with it,
  subject to Yahoo's own intraday lookback limits (it will not reach as far
  back as the stored files do). `cli_trader fetch` does not substitute — it
  produces different files (4,615 vs 4,617 bars at 5m, and differing OHLCV at
  15m even where the bar count and both endpoints match).
- **The `~full` tag is wrong for ETH.** It means "reaches further back", and
  does for 7 of 8 pairs. For ETH the pulled 4h store has 22,499 bars from
  2016-05-14 while the derived 4h has 23,282 from 2016-01-05 — the rung tagged
  `full` is 783 bars **shallower**. Both are separate manifest rows and both
  were measured, so no recorded number is wrong; the label is.
- **Derived 4h stores carry a partial trailing bar.** `resample` buckets
  unconditionally, so the last output bar is built from however many source
  bars fall in it. BTC derived 4h has 25,201 bars ending 1787241600 against the
  pulled 25,199 ending 1787212800. These are deliberately different manifest
  rungs, not two attempts at the same file. The estimator consumes an ACF over
  2000 lags and is unaffected; anything that cares about the newest bar should
  drop the trailing bucket.
- **Zero-volume bars are expected**, not corruption: 223,442 across the pulled
  stores, concentrated in thin early crypto history. Note that
  `cli_trader validate` prints only the first 40 issues and `CandleStore::load()`
  does not validate at all, so a clean-looking `validate` run is **not**
  sufficient evidence of structural integrity on the large stores.

---

## 3. How to re-run

All scripts resolve paths from their own location, so any working directory
works. Runtimes measured on this machine (Darwin, `-j8`).

```sh
# 1. build cli_trader + the four experiment helpers          (~6 s from clean)
cmake -S . -B build && cmake --build build

# 2. regenerate data/derived/** from the pulled stores       (~2 s)
python3 experiments/order_gate/build_stores.py

# 3. order-spectrum + tsmom backtest for all 98 stores       (~18 s)
python3 experiments/order_gate/gate_sweep.py run

# 4. table + Spearman correlations                           (~1 s)
python3 experiments/order_gate/gate_sweep.py report

# 5. re-score every store's pencil modes under 3 readouts    (~16 s)
python3 experiments/order_gate/im_rescore.py run

# 6. the readout head-to-head                                (~3 s)
python3 experiments/order_gate/im_rescore.py report
```

The whole pipeline runs in **under a minute** end to end. A clean rebuild is
exit 0 with zero warnings, and all four helpers compile under the project's
`CMAKE_CXX_STANDARD 17` (they were originally written against C++20 but need
nothing from it).

Both `run` subcommands **overwrite** the committed results JSONs, which are the
record of the pre-registered run. Pass `--results <path>` to write elsewhere
while exploring. `gate_sweep.py manifest` prints the store list and which files
are present.

The docstrings and `--help` text in `gate_sweep.py` claim the sweep takes
**~30 minutes**. It does not — it takes **17.6 seconds** measured end to end.
`order-spectrum` is sub-second even on the 403k-bar stores. The comment is
stale; do not budget around it.

`build_stores.py` is deterministic: regenerating all 55 derived + fw stores
into a scratch directory and comparing byte-for-byte against `data/derived/`
gives **55 identical, 0 differing**.

### Reproducibility status

I re-ran the whole pipeline from the current tree. Results:

| Path | Recorded | Fresh re-run today | |
|---|---|---|---|
| `gate_sweep report` ALL | −0.233 (p 0.057) | **+0.033 (p 0.788)** | **BROKEN** |
| `gate_sweep report` CRYPTO | −0.185 (p 0.190) | +0.201 (p 0.152) | BROKEN |
| `gate_sweep report` EQUITY | +0.115 (p 0.673) | +0.696 (p 0.004) | BROKEN |
| `im_rescore report` proj | −0.263 (p 0.031) | **−0.263 (p 0.0312)** | exact |
| `im_rescore report` re | −0.231 (p 0.059) | **−0.231 (p 0.0587)** | exact |
| backtest fields (98 rows) | — | 0 differences | exact |

**Cause.** `gate_sweep.parse_spectrum` reads the spectrum table positionally:

```python
tail_sprd, tail_mean = toks[-2], toks[-1]
```

When the recorded run was made, that row ended `… spread tailSprd tailMean` —
9 tokens. The estimator upgrade (section 6) added a **`tailBeta`** column, so
the row now has 10:

```
  series                    r=1    r=0.3    r=0.1   r=0.03   r=0.01   spread  tailSprd  tailMean  tailBeta
  BTC_USDT               -0.500   -0.666   -0.740   -0.739   -0.745    0.245     0.006    -0.741     0.007
```

Everything shifts by one. `tail_spread` now silently receives `tailMean`
(−0.741) and `alpha` receives `tailBeta` (0.007). Because `tail_spread` is now
negative, `ref_tail > 0` fails and `ratio` becomes `nan` for most rows — and
`nan` propagates silently, since `nan != nan` comparisons never flag.

**What still works.** Only the spectrum half broke. `parse_backtest` is
unaffected: `excess_sharpe`, `trades`, `sharpe`, `bench_sharpe` and `candles`
reproduce with **zero** differences across all 98 rows. `im_rescore.py` does
not use `parse_spectrum` at all — it runs the `spiral_modes` helper and
computes every ratio from the raw pencil modes — which is why the headline
survives untouched.

The built-in tripwire fired correctly: `im_rescore`'s re-readout cross-check
reports a max deviation of **5.6703** against a fresh sweep, versus **0.0005**
in the recorded file.

**The fix** is two lines, verified in a scratch copy — shift both the symbol
row and the power-law reference rows by one:

```python
tail_sprd, tail_mean = toks[-3], toks[-2]     # was toks[-2], toks[-1]
...
if line.strip().startswith("power law") and len(toks) >= 4 and toks[-3] != "n/a":
    ref_tails.append(float(toks[-3]))         # was toks[-2]
```

With that patch `gate_sweep report` gives **−0.266 (p 0.031)**, and its ratios
agree with `im_rescore`'s `proj_ratio` to within 0.036 (CLI rounds to 3dp).

**But note what that means.** Even patched, the recorded **−0.233 can never be
regenerated**, because the shipping binary now prints the *projection* α where
it used to print the *Re* α. The `ratio` column in `results/gate_results.json`
is a historical artifact of the pre-upgrade estimator. That is not a bug — it
is the pre-registered "before" arm, preserved. It is the reason
`im_rescore.py`, which recomputes all three readouts from raw modes, is the
authoritative comparison and `gate_sweep.py`'s own correlation is not.

I did not patch the shipped script: it is the code that produced a
pre-registered result, and changing what it reports is a decision for whoever
owns the experiment, not the person documenting it.

---

## 4. The result

Statistics run on the **68 rows with ≥ 8 trades** (Sharpe is meaningless below
that). `fw` rows and the duplicate full-depth 4h rows are excluded; the 1d
`~full` rows are kept, having no untagged twin. These filters are
pre-registered — do not tune them.

Rank correlation of identification ratio against `tsmom` excess Sharpe,
prediction **negative**, permutation p over 20,000 shuffles at a fixed seed:

| Readout | α from | Spearman | perm p |
|---|---|---:|---:|
| `re` | Re(ω)/σ | −0.231 | 0.059 |
| `im` | Im(ω) | −0.249 | 0.042 |
| **`proj`** | **(σ·Re + Im)/(1+σ²)** | **−0.263** | **0.031** |

Split by asset class, the whole effect is crypto:

| Subset | n | `re` | `proj` | `im` |
|---|---:|---:|---:|---:|
| crypto | 52 | −0.185 (p 0.188) | −0.218 (p 0.118) | −0.209 (p 0.134) |
| equity | 16 | +0.115 (p 0.668) | −0.038 (p 0.890) | +0.038 (p 0.883) |

Neither crypto subset is significant on its own. The equity subset is noise in
both directions — 16 rows, most of them on 12-week windows.

Splitting on the categorical verdict rather than the continuous ratio is much
weaker: identified n=35, mean excess −0.17, versus not-identified n=33, mean
excess −0.28. Both negative. **The gate concentrates winners; it does not
manufacture them.**

### Concentration

Base rate across the 68 rows: **21 positive-excess rows = 30.9%**. Universe
mean excess **−0.223**. Top decile = the 7 best-ranked rows.

| Ranking | top-7 positive | top-7 mean excess |
|---|---:|---:|
| `re` ratio | 4/7 = **57.1%** | **+0.101** |
| `proj` ratio | 3/7 = **42.9%** | **+0.020** |

> **Discrepancy — flagged.** The project's research notes state "the top decile of
> the ranking holds 58% positive-excess rows vs a 31% base rate; its mean
> excess is +0.09 vs −0.22", quoting it alongside the projection headline. The
> base rate (30.9%) and universe mean (−0.223) confirm. **The decile figures do
> not.** 58% / +0.09 matches the **`re`** ranking (57.1% / +0.101), not the
> **`proj`** ranking that shipped, which gives **42.9% / +0.020**. Under `proj`
> no cutoff between k=4 and k=25 reaches 58% — the maximum is 50% at k=12. The
> decile statistic and the headline correlation were computed under **different
> readouts** and should not be quoted as one finding.

### Shortlist

Positive-excess rows inside the best-ranked 8 by `proj_ratio`:

| Row | proj ratio | excess Sharpe | ±1 s.e. | trades | span | max DD | B&H Sharpe |
|---|---:|---:|---:|---:|---:|---:|---:|
| BTC_USDT @ 4h | 0.081 | **+0.21** | 0.29 | 88 | 11.50 y | 39.7% | 1.06 |
| TRX_USDT @ 1h | 0.294 | **+0.15** | 0.38 | 88 | 6.77 y | 44.1% | 0.90 |
| BTC_USDT @ 2h | 0.258 | **+0.10** | 0.29 | 130 | 11.50 y | 36.4% | 1.04 |
| TRX_USDT @ 4h | 0.259 | **+0.10** | 0.38 | 43 | 6.77 y | 47.4% | 0.93 |

All four excesses are **smaller than their own standard errors.** +0.21 ± 0.29
is not distinguishable from zero on a single series. The shortlist is a ranking,
not a set of significant findings.

The 5m positives (XRP_USDT +0.74 on 10 trades, LTC_USDT +0.34 on 18) sit just
over the trade threshold and carry too few trades to weight.

Verified directly against the shipping binary:

```
$ ./build/cli_trader order-spectrum --symbol BTC_USDT --period 14400
  BTC_USDT   -0.500  -0.666  -0.740  -0.739  -0.745   0.245   0.006  -0.741   0.007
  FRACTIONAL order identified: alpha = -0.741, stable to 0.006

$ ./build/cli_trader backtest --symbol BTC_USDT --period 14400 \
      --strategy tsmom --vol-target 0.20 --profile crypto
  Sharpe (ann.):   1.28 +/- 0.29     1.06     0.21
```

---

## 5. The caveats

Read these as part of the result, not as fine print.

**The rows are not independent, so the effective n is well below 68.** Each
instrument contributes 6–7 timeframe rungs, and by construction those rungs are
resampled from *the same underlying bars* — a pair's 30m through 4h rungs are
literally aggregations of its one 15m store. 68 rows come from 13 instruments.
The permutation test shuffles rows as if they were exchangeable; they are not.
p = 0.031 is therefore **optimistic by an unquantified amount**, and the honest
reading is "suggestive at 13 effective units", not "significant at 68".

**The ratio is substantially a sample-size statistic.** This is the sharpest
confound and it is larger than the effect being reported:

| Correlation | Spearman |
|---|---:|
| proj ratio vs **bar count** | **−0.533** |
| proj ratio vs calendar span (years) | −0.045 (p 0.72) |
| excess Sharpe vs bar count | −0.103 |

More bars → lower (better) ratio, at twice the magnitude of the headline
result. Young or thinly-sampled series get penalised on data quantity, not on
memory structure.

Two corrections to how this has been described. First, the driver is **bar
count, not calendar age**: span in years is uncorrelated with the ratio
(−0.045, p 0.72), and within crypto alone the sign actually flips (+0.158).
Naming ADA and SOL as the penalised cases is a reasonable shorthand but the
mechanism is sample size — LTC, with 11.45 years, has the *worst* mean proj
ratio of any pair (1.52).

Second, and in the experiment's favour: **the confound works against the
hypothesis, not for it.** Partialling bar count out of the headline
correlation *strengthens* it, from −0.263 to **−0.377** (the `re` readout goes
−0.231 → −0.323). Because excess Sharpe is itself mildly *negatively*
correlated with bar count, sample size is suppressing the relationship rather
than manufacturing it. The confound is real and must be controlled, but it is
not the explanation for the finding.

**Most (instrument, timeframe) pairs still lose to buy-and-hold after fees.**
69% of the 68 rows have negative excess Sharpe; the universe mean is −0.223.
The best-ranked decile has a mean excess of +0.02 under the shipped readout.
This experiment moves probability around inside a losing population — it does
not turn it into a winning one.

**The shortlist is not out-of-sample.** The spectra that ranked those four rows
were computed on the **full sample**, and the backtests that scored them cover
the **same bars**. The gate has never been tested the way it would be used:
fit the spectrum on a trailing window, select instruments, trade forward, roll.
**A walk-forward test of the gate itself is the outstanding next step**, and
until it exists the correct description of this result is "a full-sample rank
correlation with a plausible mechanism", not "an edge".

**Equity contributes essentially nothing.** 16 rows on windows of 12 weeks
(5m/15m) to 2.9 years (1h), with the price-definition mismatch described in
section 2. Its correlations are noise at both signs.

---

## 6. What changed in the estimator

The experiment changed what `src/math/spiral.cpp` ships.

**The readout.** The spiral model says ω = (σ + i)·α with α real, so
`Re(ω) = σ·α` and `Im(ω) = α` are two *independent* readings of the same order.
The estimator originally read α from Re alone — which divides by σ = 0.3 and
amplifies mode noise 3.3-fold. The pre-registered alternative was the
least-squares projection of ω onto the model direction (σ, 1):

```
α = (σ·Re(ω) + Im(ω)) / (1 + σ²)
```

**The head-to-head was declared before it was run**, on a fixed row set (≥ 8
trades, `fw` and duplicate 4h rows excluded) and a fixed test (Spearman,
20,000-shuffle permutation, seed 7): the readout with the larger |Spearman|
carrying the predicted negative sign wins. `proj` won, −0.263 (p 0.031) against
`re` −0.231 (p 0.059). `im` placed between them at −0.249.

That is a genuine but **narrow** win — 0.032 of Spearman on 68 non-independent
rows, with both readouts computed on the same modes and therefore heavily
correlated. It is enough to prefer `proj`; it is not enough to call `re`
refuted. The mechanical argument (don't divide by σ) is the stronger reason.

The improvement in radius stability is much less ambiguous than the correlation
margin. On BTC 4h the identification tightened from spread **0.087 to 0.006**
against a reference of 0.082 — roughly 14x, and tighter than the *true*
synthetic power law measured at identical settings.

**The beta consistency check.** The component perpendicular to the model
direction,

```
β = (σ·Im(ω) − Re(ω)) / (1 + σ²)
```

is the *complex* part of the order, and it is a sanity check rather than a
predictor. A genuinely real order pins β ≈ 0 — synthetic power laws stay within
|β| ≤ 0.003 across a decade of radii, while exponentials leak up to ~0.04. A
stable α sitting *off* the real-order manifold means log-periodic modulation
(discrete scale invariance, ACF ~ k^−(α+1)·cos(β·ln k + φ)), which is a
different model, not a fractional order.

So the verdict now requires **both** radius-stability of α **and** |β| inside
the reference-calibrated tolerance, and a stable α with off-manifold β is
reported as a **COMPLEX** order rather than a fractional one. On BTC 4h,
β = +0.007 — on the manifold. The check holds across the best-ranked rows:

| Row | proj ratio | β |
|---|---:|---:|
| BTC_USDT @ 4h | 0.08 | +0.006 |
| TRX_USDT @ 2h | 0.15 | −0.000 |
| TRX_USDT @ 30m | 0.22 | +0.001 |
| BTC_USDT @ 2h | 0.26 | −0.002 |
| TRX_USDT @ 4h | 0.26 | −0.001 |

The rows the gate likes are on the real-order manifold, which is the
self-consistency the check exists to test.

This is also the change that broke `gate_sweep.py`: exposing β added the
`tailBeta` column that shifted the parser.

---

## Files

| Path | What |
|---|---|
| `order_gate/build_stores.py` | resamples `data/` → `data/derived/**`; reads pulled stores, never writes them |
| `order_gate/gate_sweep.py` | manifest, sweep runner, report; owns the `LADDER` and the permutation test |
| `order_gate/im_rescore.py` | the readout head-to-head; authoritative for the correlations |
| `order_gate/results/gate_results.json` | recorded sweep, 98 rows (`ratio` column is pre-upgrade) |
| `order_gate/results/im_results.json` | recorded re-scoring, 98 rows, all three readouts |
| `tools/storeinfo.cpp` | `<symbol> <period> <n> <first_ts> <last_ts>` |
| `tools/resample.cpp` | bucket a store to a coarser period (partial edge buckets — see section 2) |
| `tools/keeplast.cpp` | keep last N bars, or all bars at/after an epoch (arg ≥ 1e9 switches mode) |
| `tools/spiral_modes.cpp` | emits raw pencil modes as JSON; replicates `cmdOrderSpectrum`'s construction exactly |

The tools live **outside `src/`** deliberately: the root `CMakeLists.txt` does
`file(GLOB_RECURSE CLI_SOURCES CONFIGURE_DEPENDS src/*.cpp)`, so any `.cpp`
with a `main()` under `src/` breaks the build with duplicate `main` symbols.
They are wired in via `add_subdirectory(experiments/tools)` and build to
`build/experiments/tools/`.

## Outstanding

1. **Walk-forward the gate.** The single thing that would turn this from a
   correlation into a result.
2. **Fix `gate_sweep.parse_spectrum`** (two-line patch above), or better, make
   it parse by column header instead of by negative index so the next estimator
   change fails loudly instead of silently.
3. **Port the equity intraday fetcher** into `experiments/fetch/`. Until then
   14 pulled stores are unreproducible.
4. **Decide which 4h store is canonical** for crypto — pulled or derived — and
   fix the `~full` tag, which is backwards for ETH.
5. **Re-derive the decile statistic** under the shipped readout wherever it is
   quoted, or label it as the `re`-readout figure it actually is.
