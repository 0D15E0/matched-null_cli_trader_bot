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
