# The Ensemble with Causal Inverse-Volatility Weighting

This document describes the research variant in plain English. It is not a
live deployment instruction. The live book currently uses equal sleeve weights;
the inverse-volatility version is an experiment.

## The short version

The system watches eight crypto markets and three trend rules. Every bar, each
market asks:

> Is the trend strong enough that I should be holding this coin rather than
> cash?

The three rules are the following concrete implementations. The defaults are
bar-based; the parenthetical translations assume the live 4h bars.

- **Time-series momentum (`tsmom`):** calculate the close-to-close return over
   the last 90 bars (about 15 days on 4h data). Vote long when that return is
   above +5%. Vote against the position when it falls below -5%. A new long
   entry is also blocked when the recent 30-bar realized volatility is above
   the 60% annualized ceiling; the volatility gate never prevents an exit.
- **Faber moving average (`faber_ma`):** calculate the 200-bar simple moving
   average of the close (about 33 days on 4h data). Vote long when the current
   close is above that average and vote out when it is below. The default band
   is 0%, so there is no extra buffer around the average.
- **Donchian breakout (`donchian`):** compare the current close with the
   previous 55 bars, excluding the current bar. Vote long only after a close
   above that prior high (about 9 days on 4h data). While long, vote out after
   either a close below the previous 20-bar low (about 3.3 days) or a close
   below the highest close reached during the trade minus 2.5 times the current
   20-bar ATR. The first exit condition reached wins.

The exact default parameters are:

| member | entry condition | exit condition | default bars |
|---|---|---|---:|
| `tsmom` | 90-bar close return > +5%, with 30-bar annualized volatility <= 60% | 90-bar close return < -5% | 90 / 30 |
| `faber_ma` | close > 200-bar SMA | close < 200-bar SMA | 200 |
| `donchian` | close > exclusive 55-bar high | close < exclusive 20-bar low, or 2.5 ATR trailing stop | 55 / 20 / 20 |

These are trend-following long/flat rules, not three independent portfolio
allocations. Each member is replayed with its own virtual position so that its
vote reflects whether that rule would currently still be long. A member's
entry and exit are therefore stateful even though the resulting vote is
recomputed causally from the candle history.

The market is eligible for a long position when at least two of the three rules
vote long. It leaves the position only when zero rules still vote long. This is
the `ensemble_vote` configuration `enterVotes=2,exitVotes=0`. The engine still
fills the resulting ensemble signal at the next bar's open. During the
position, one member may already have exited while the other two keep the
ensemble invested; an exit happens only after the last member has stopped being
long.

The strategy is long-only. When a market does not qualify, its sleeve holds
cash. It never shorts and never borrows.

## What inverse-volatility weighting means

There are two different sizing decisions:

1. **Inside a sleeve:** the strategy sizes a position toward a target annual
   volatility, such as 20% or 30%, using only information available before the
   entry. This controls how much of a particular coin the sleeve holds.
2. **Across sleeves:** the portfolio decides how much of the total book belongs
   to each coin. Inverse-volatility weighting gives a smaller budget share to a
   coin that has recently moved violently and a larger share to a calmer coin.

In symbols, if the recent volatility estimate of sleeve $i$ is $sigma_i$, its
raw portfolio weight is proportional to $1 / sigma_i$:

$$
w_i = \frac{1 / sigma_i}{\sum_j 1 / sigma_j}
$$

The weights are normalized so they add to 100%. The portfolio therefore does
not create leverage just because one market is calm.

## Why it is causal

At a scheduled rebalance, the portfolio looks back over a fixed number of
already-observed sleeve returns. It calculates each trailing volatility, forms
the inverse-volatility weights, and holds those weights until the next
rebalance. The current or future return is never included in the estimate.

For example, the live-scale 4h experiment uses a 120-bar volatility lookback
and refreshes every 30 bars. That is approximately 20 days of history and a
5-day refresh interval. A 15-day / 5-day version uses 90 / 30 bars on 4h data.
The multi-timeframe sweep converts these windows from calendar days into bars,
so the same research question means the same amount of time at 30m, 1h, 2h,
4h, and 1d.

## What it is trying to improve

Equal weighting treats a quiet coin and a violently moving coin as equally
important. That can make the total portfolio risk depend too heavily on the
most turbulent sleeve. Inverse-volatility weighting tests a simple alternative:

- reduce the budget of recently turbulent sleeves;
- give relatively more budget to calmer sleeves;
- keep the total gross budget at 100%;
- leave the trading signal unchanged.

This isolates portfolio construction from signal selection. A result cannot be
credited to inverse-volatility weighting if the trend rules or their thresholds
also changed.

## Research design

The fixed exploratory sweep is
`experiments/ensemble_invvol_timeframe_sweep.py`. It tests:

- eight symbols: BTC, ETH, XRP, LTC, DOGE, TRX, ADA, and SOL;
- derived 30m, 1h, 2h, 4h, and 1d stores;
- development data from 2021-01-01 through 2023-12-31 only;
- a 240-bar warm-up that is not scored or traded;
- 7, 15, 20, and 30 calendar-day volatility memories;
- 3, 5, and 10 calendar-day rebalance intervals;
- 20% and 30% sleeve volatility targets.

Run it with:

```sh
python3 experiments/ensemble_invvol_timeframe_sweep.py
```

The output is written outside the repository by default to
`/tmp/ensemble_invvol_timeframe_sweep.json`. It records the exact command,
timeframe, windows, common-bar count, Sharpe standard error, excess Sharpe,
and close-to-close maximum drawdown for every candidate.

The first fixed grid completed all 40 candidates successfully. Its best row per
timeframe was:

| timeframe | memory | rebalance | target | excess Sharpe | Sharpe SE | max drawdown | common bars |
|---|---:|---:|---:|---:|---:|---:|---:|
| 30m | 15d | 5d | 20% | 0.28 | 0.71 | 18.46% | 34,733 |
| 1h | 15d | 5d | 20% | 0.72 | 0.71 | 13.27% | 17,367 |
| 2h | 30d | 10d | 20% | 0.70 | 0.71 | 13.56% | 8,685 |
| 4h | 20d | 5d | 20% | **1.16** | 0.71 | **14.78%** | 4,343 |
| 1d | 7d | 3d | 20% | -0.35 | 0.71 | 29.10% | 725 |

The 4h result was the strongest row in the grid. The same 20-day/5-day memory
at the 30% target produced excess Sharpe `1.03` and max drawdown `21.41%`, so
the lower target reduced risk and improved this sample's risk-adjusted result.
The result is not statistically established: the standard error is 0.71 and
the grid itself creates selection pressure. The 1d result also has only 725
common bars, so it is especially weak evidence despite being a complete run.

The sweep is discovery evidence. A strong row must be given a new hypothesis
ID and evaluated under the registry's kill rules before it can be considered
for any later forward test. Looking at a result and changing its window is a
new hypothesis, not a continuation of the old one.

## Benchmark

Every portfolio result is compared with an equal-weight buy-and-hold basket of
the same eight instruments on the same common timestamp grid. The benchmark
uses the same next-open entry, fee, slippage, close marking, and final
liquidation conventions. The primary comparison is excess annualized Sharpe,
not raw return. Portfolio drawdown is close-to-close and is labelled as such.

No result in this document is a claim that inverse-volatility weighting has a
future edge. It is a transparent, causal risk-allocation experiment whose
next gate is chronological stability and then the untouched forward period.