# `order-spectrum`: what order is a series' memory?

Reference for the `order-spectrum` command, which applies a logarithmic-spiral
order estimator to a series' autocorrelation and asks which model class its
memory belongs to. The estimator lives in `src/math/spiral.h` and
`src/math/pencil.h`.

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
[experiments/](../experiments/); the honest summary is that the raw headline does
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
