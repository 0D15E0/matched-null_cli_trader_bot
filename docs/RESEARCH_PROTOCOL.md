# Research protocol

How a hypothesis gets registered, evaluated once, and killed or kept. The point
of the protocol is that a candidate cannot be quietly retried until it passes:
seeing a result and changing a parameter requires a new hypothesis ID.

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
