# Evolutionary search, and why its early results meant nothing

`evolve` searches one strategy's parameters; `evolve-strategy` searches the
trading rules themselves. Both were well built and, for a long time, measured
the wrong thing. This is the record of what was wrong and what the fitness
function does now.

## Evolution, and why its old results were meaningless

`evolve` searches `OdiseoParams`; `evolve-strategy` searches the trading rules
themselves (genetic programming over a scale-invariant feature set). The
machinery was always well built. What was wrong was everything around it:

- Fitness maximized a t-statistic that scaled with sample length, so the
  "minimum across environments" was really comparing a 25,000-bar crypto series
  against a 2,500-bar equity one — incommensurable numbers.
- It scored raw performance on assets that rose 15x-280x, so "generalize across
  environments" collapsed into "stay long in uptrends."
- The drawdown penalty subtracted 0.75 for a 75% drawdown against a Sharpe term
  on a ~4.0 scale, so selection happily accepted ruinous drawdowns.
- Results were reported on the same candles fitness was maximized over, after
  trying thousands of candidates.

Now: fitness is **excess annualized Sharpe over buy-and-hold minus a quadratic
drawdown penalty**, minimized across environments, and validation is
walk-forward.

```sh
./build/cli_trader evolve-strategy \
    --envs BTC_USDT:14400,ASML.AS:86400,AAPL:86400 \
    --population 24 --generations 12 --wf-folds 3 --vol-target 0.20
```

Anchored walk-forward: fold *k* trains on everything up to a boundary and is
validated only on the window that follows. A real 3-fold run produced:

```
===== Out-of-sample verdict =====
Segments evaluated:     9
Mean excess CAGR:       -23.09%
Worst excess CAGR:      -79.14%
Beat buy & hold in:     1/9 segments

VERDICT: no out-of-sample edge over buy-and-hold. Do not deploy this.
```

— while the same genome's **in-sample** report looked strong (Sharpe 1.26 vs
buy-and-hold's 1.05, drawdown 34% vs 84%). That gap is the entire lesson of this
project, and the tool now prints it unprompted.

The verdict is judged on **excess Sharpe, not excess return**, because a
vol-targeted strategy deliberately runs at partial exposure and would lose on
raw CAGR to a fully-invested benchmark even when it is the better bet per unit
of risk. It reports a standard error across segments, requires at least six
segments, and will not call anything deployable unless the mean excess clears
two standard errors.

Test windows carry an indicator warm-up prefix that feeds the indicators without
being traded or scored. Without it, a fold sliced flush at the boundary left
`odiseo` blind for ~80 bars while buy-and-hold was invested from bar one — a
systematic bias against the strategy in the one measurement that decides
everything.

The deflated Sharpe flags overfitting independently. Every report from a search
shows:

```
Search correction (288 candidates evaluated):
  Sharpe expected from luck alone: 0.85
  Deflated Sharpe (P[true SR > that]): 0.917  <- NOT significant
```

Saved genomes carry honest provenance: how many candidates were tried, that the
genome was fitted on **all** the data, and that the walk-forward verdict judged
the *search procedure* rather than the saved artifact. A genome fitted on
everything is never stamped "validated".

Requesting validation that cannot be built is a hard error, not a silent
downgrade: a `--train-end` that leaves too little data on either side used to
hand back an essentially random genome and report it as an out-of-sample result.

> The genome files that used to be checked in (`emergent_genome*.json`,
> `emergent_bear_2022.json`, per-symbol ones) were produced by the **old**,
> broken fitness function and had no holdout. They were **deleted on
> 2026-08-26** — superseded by the `tournament` work, whose results and
> methodology live in [TOURNAMENT.md](TOURNAMENT.md) and `tournament_results/`.
> They remain in git history (commit `08f689b`) if a genome is ever needed for
> forensics. `evolve-strategy --save` still writes to `emergent_genome.json` by
> default, and `backtest --strategy emergent` fails with a clear message when
> the file is absent.
