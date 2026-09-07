# Indicator semantics and corrections

Several indicators here once disagreed with their own documentation. All are
fixed, which means parameters tuned against the old versions do not transfer.
This is the current definition of each, and what it used to be.

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
