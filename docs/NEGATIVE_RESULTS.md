# Things that were measured and did not work

The useful half of the research record. Each of these was built, measured
properly, and rejected. Several are cases where a genuinely better *instrument*
produced a worse *strategy*, which is the most reliable regularity in this
project.

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
