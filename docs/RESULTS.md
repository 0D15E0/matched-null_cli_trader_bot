# Measured results

What the engine reports for a few strategies on the shipped stores, and how the
portfolio benchmark is constructed. Every number here is a measurement on
historical data with fees and slippage charged, not a claim about the future.
Read [NEGATIVE_RESULTS.md](NEGATIVE_RESULTS.md) alongside it: most of what was
tried did not work, and that is the more useful half.

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

### Volatility targeting is a leverage dial

The claim that volatility targeting "is the one result that reproduces" needs
to be stated precisely, because it is easy to read as "it improves Sharpe". It
does not. What it does is let you choose your drawdown.

Four protocol coins (BTC, ETH, XRP, LTC at 4h), equal weight, 2018-01-01 to
2023-12-31, 600 warm-up bars, 30-bar volatility window, fees and slippage at
the defaults. Only `--vol-target` changes between rows.

| vol target | `tsmom` Sharpe | CAGR | max DD | | `ensemble_vote` Sharpe | CAGR | max DD |
|---|---|---|---|---|---|---|---|
| all-in | 1.07 | 36.6% | 47.4% | | 1.04 | 40.7% | 42.5% |
| 0.10 | 1.13 | 10.9% | 12.0% | | 1.05 | 9.8% | 9.6% |
| 0.15 | 1.15 | 16.0% | 17.4% | | 1.06 | 14.4% | 14.1% |
| 0.20 | 1.16 | 20.7% | 22.4% | | 1.08 | 18.7% | 18.4% |
| 0.30 | 1.12 | 27.3% | 31.3% | | 1.05 | 24.7% | 27.0% |
| 0.40 | 1.08 | 31.1% | 38.7% | | 1.01 | 28.3% | 34.1% |

Sharpe standard error on this window: ±0.41. Every Sharpe in the table is
inside one standard error of every other in its column. Return and drawdown,
meanwhile, move almost linearly with the target, until the 100% exposure cap
bends the top of the ladder: CAGR per unit of target falls from about 1.1 at
0.10 to about 0.8 at 0.40, and drawdown starts growing faster than return.

Two consequences follow. The target is a risk setting, not a signal claim, so
choosing it is a decision about how much drawdown you are willing to hold, and
nothing else. And **two configurations at different vol targets cannot be
compared on Sharpe, return, or drawdown alone**; the standing rule in this
repository is to de-lever the stronger one to the other's drawdown first, then
compare. Most "improvements" that skip that step are the dial being turned.

### Diversification is arithmetic

Same setup as the ladder, `tsmom` at vol target 0.20, each coin traded alone
and then all four as an equal-weight portfolio. The portfolio report prints the
number this subsection is about, so what follows is the tool's own output, not
an interpretation of it.

| | Sharpe | max DD | trades |
|---|---|---|---|
| BTC alone | 1.30 | 39.7% | 46 |
| ETH alone | 0.74 | 34.8% | 46 |
| XRP alone | 0.77 | 38.5% | 41 |
| LTC alone | 0.65 | 27.6% | 37 |
| mean of the four | 0.87 | | |
| **equal-weight portfolio of the four** | **1.16** | 22.4% | 170 |
| equal-weight buy-and-hold basket of the four | 0.51 | 88.9% | |

Average pairwise sleeve correlation: 0.43. Four sleeves with a mean Sharpe of
0.87 and that correlation should combine to about 1.15 (mean × √(N ⁄ (1 +
(N−1)ρ))). Achieved: 1.16. Nothing about the future went into that prediction;
only the sleeves' standalone quality and how much they move together.

Two honest notes. The portfolio does **not** beat its best sleeve: BTC alone
scored 1.30. But you would have had to know in advance which sleeve that would
be, and [TOURNAMENT.md](TOURNAMENT.md) is the record of why you cannot. What
diversification buys is the mean plus a third, with no forecast required. And
the per-sleeve drawdowns are intrabar-aware while the portfolio's is
close-to-close, so the 22.4% is not directly comparable with the rows above it;
the report says so, and the fair comparison is the Sharpe column.

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
