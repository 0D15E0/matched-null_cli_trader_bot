#!/usr/bin/env python3
"""Walk-forward test of the order gate: does it pick instruments CAUSALLY?

THE QUESTION THIS ANSWERS, AND WHY THE EARLIER SWEEP DID NOT
------------------------------------------------------------
gate_sweep.py measured the gate's ratio and tsmom's excess Sharpe on the SAME
full history, then correlated them.  That establishes an association; it cannot
establish that the gate would have helped, because the ranking it produces was
computed with knowledge of the whole sample.  Any shortlist read off that table
is selected in-sample, and this repo exists largely as a monument to how badly
that flatters a strategy.

This script removes that objection.  At each fold boundary T it:

  1. ranks instruments using ONLY bars before T     (order-spectrum --end T)
  2. selects the better-ranked half
  3. measures what those instruments did AFTER T    (backtest --start T --end T')

so every selection decision is made from information that existed at the time.
The comparison is selected-half vs the whole universe vs the rejected half, plus
a random-selection null of the same size.

WHAT IS BEING SELECTED, AND WHY IT IS INSTRUMENTS
-------------------------------------------------
The in-sample decomposition (see gate_sweep.py report) found the gate's signal
lives entirely in one dimension: ranking INSTRUMENTS at a fixed timeframe gives
a pooled rho of -0.32, negative in all 7 timeframes, while ranking TIMEFRAMES
for a fixed instrument gives ~0.00.  So the rule under test here is "at a
timeframe you have already chosen, trade the instruments whose memory the
estimator can resolve", and each timeframe is tested as its own experiment.

CAUSALITY, CONCRETELY
---------------------
`order-spectrum --end T` slices the series before computing anything - the
autocorrelation, the contour and the pencil never see a bar after T.  Verified
empirically: its output is identical, to the last printed digit, to running the
command against a store from which every later bar has been physically deleted.
`backtest --start T --end T'` scores only bars in the window, using a warm-up
prefix from before T to fill indicators (the engine's evaluateFromTimestamp),
which is a look-BACK, not a look-ahead.

ELIGIBILITY
-----------
An instrument enters a fold only if it has at least --min-train-bars before T,
which is what a live system would require before trusting an estimate.  Young
instruments therefore appear in later folds only - exactly as they would in
practice - instead of being dropped from the study or, worse, being ranked off
an estimate built from 300 bars.

RESULT: THE GATE FAILED THIS TEST.  READ THIS BEFORE USING THE SCRIPT.
---------------------------------------------------------------------
Run: 3 timeframes (1h, 2h, 4h) x 5 folds, 121 (instrument, fold) decisions.
The headline looked like a pass - the selected half beat the universe by +0.203
excess Sharpe, with the random-selection null at p = 0.008 (2h) and 0.015 (4h).
It is an artifact, and the mechanism is embarrassing but instructive.

The 5 equities become eligible only in the final fold.  The gate REJECTED 87% of
them against 46% of crypto, and equities happened to average -0.826 excess
Sharpe against crypto's -0.132.  So a rule that merely says "avoid the
equities" reproduces the entire edge without any ranking skill whatsoever.
Strip the confound and nothing is left:

    all 13 instruments, top-half            +0.203   (null p 0.001)
    crypto only, re-ranked                  +0.089   (null p 0.084)
    balanced panel (always-eligible names)  +0.012   (null p 0.427)
    fold 4, crypto only                     -0.006

Three further checks agree it is not real:
  * The edge exists ONLY at top-half and top-two-thirds.  Every more selective
    rule fails: top-1 +0.066 (p 0.363), top-3 +0.104 (p 0.108), top-quartile
    +0.084 (p 0.216).  A genuine ranking signal is STRONGEST at the top; this
    one appears only once you are selecting most of the universe, which is what
    a group-separating artifact looks like, not a ranking.
  * Spec-mining: a RANDOM per-fold ranking, evaluated at all 7 selection sizes
    and scored at its best, reaches +0.203 in 15.9% of draws.  Search-adjusted
    p = 0.159.
  * On a homogeneous universe the ranking is near-arbitrary at every timeframe:
    fold-stratified AUC crypto-only is 0.477 (2h), 0.627 (4h), 0.500 (1h).

WHY the equities were flagged is a defect in this repo's own data, not a market
property: their 2h/4h stores are resampled from 1h bars onto a UTC grid that
cuts across 6.5-hour trading sessions, so the buckets are ragged and the
autocorrelation - hence the order estimate - is distorted.  The gate detected
our resampling, not their memory.

And 1h, which looked like the disappointing timeframe, was the honest one: its
in-sample rho of -0.60 was the best of seven strata, and P(min rho <= -0.604
across 7 strata) = 0.26 under the null.  There was never anything there to
replicate.

The mechanics of the test are sound - that part was verified hard.  Five
truncation tests confirmed `order-spectrum --end T` is byte-identical to
running against a store with the later bars physically deleted; the backtest
window is likewise unaffected by post-window bars; positive controls confirmed
those tests can detect a planted leak; and all 121 rows replay exactly.  The
experiment is correct.  The hypothesis is what failed.

USAGE
    python3 walk_forward_gate.py run --timeframes 3600 --folds 4
    python3 walk_forward_gate.py report
"""
from __future__ import annotations

import argparse
import json
import math
import os
import random
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gate_sweep as gs  # noqa: E402  (layout, parsers and stats live there)

DEFAULT_RESULTS = HERE / "results" / "walk_forward.json"


def eligible_stores(layout, period):
    """Every (symbol, path) that has a store at this period, using the same
    store-kind mapping the sweep pre-registered."""
    out = []
    for profile, syms in gs.SYMBOLS.items():
        for sym in syms:
            for per, kind, tag in gs.LADDER[profile]:
                if per != period or tag:      # untagged rungs only
                    continue
                path = Path(gs.store_path(layout.dir_for(kind), sym, per))
                if path.exists():
                    out.append((sym, profile, path, layout.dir_for(kind)))
    return out


def store_span(layout, path):
    p = subprocess.run([str(layout.repo / "build" / "experiments" / "tools" / "storeinfo"),
                        str(path)], capture_output=True, text=True)
    if p.returncode != 0:
        return None
    f = p.stdout.split()
    return int(f[2]), int(f[3]), int(f[4])      # nbars, first_ts, last_ts


def measure_ratio(layout, sym, period, datadir, train_end, cache):
    """Gate ratio from bars strictly before `train_end`.  THE CAUSAL STEP."""
    key = f"ratio:{sym}:{period}:{train_end}"
    if key in cache:
        return cache[key]
    out = gs.run_cmd(layout, [layout.cli, "order-spectrum", "--symbol", sym,
                              "--period", str(period), "--data-dir", str(datadir),
                              "--end", str(train_end)])
    spec = gs.parse_spectrum(out, sym)
    if spec.get("parse_error"):
        raise SystemExit("order-spectrum parse failure: %s" % spec["parse_error"])
    val = dict(ratio=spec.get("ratio"), alpha=spec.get("alpha"),
               verdict=spec.get("verdict"), candles=spec.get("candles"))
    cache[key] = val
    return val


def measure_forward(layout, sym, profile, period, datadir, start, end, cache):
    """tsmom performance in [start, end) - strictly after the ranking."""
    key = f"fwd:{sym}:{period}:{start}:{end}"
    if key in cache:
        return cache[key]
    out = gs.run_cmd(layout, [layout.cli, "backtest", "--symbol", sym,
                              "--period", str(period), "--strategy", "tsmom",
                              "--vol-target", "0.20", "--profile", profile,
                              "--data-dir", str(datadir),
                              "--start", str(start), "--end", str(end)])
    bt = gs.parse_backtest(out)
    val = dict(excess=bt.get("excess_sharpe"), sharpe=bt.get("sharpe"),
               bench=bt.get("bench_sharpe"), trades=bt.get("trades", 0),
               maxdd=bt.get("maxdd"), ok=bt.get("bt_ok", False))
    cache[key] = val
    return val


def cmd_run(args):
    layout = gs.Layout(args.repo, args.data_dir, args.derived_dir, args.fw_dir, args.cli)
    cache_path = Path(args.cache)
    cache = json.loads(cache_path.read_text()) if cache_path.exists() else {}
    results = []

    for period in args.timeframes:
        stores = eligible_stores(layout, period)
        if len(stores) < 4:
            print(f"[{period}s] only {len(stores)} stores, skipping")
            continue
        spans = {}
        for sym, profile, path, datadir in stores:
            sp = store_span(layout, path)
            if sp:
                spans[sym] = sp

        # Fold boundaries on a COMMON calendar, so "rank at time T" means the
        # same T for every instrument.
        #
        # The grid does NOT span the widest history: anchoring it at the oldest
        # instrument put the early boundaries in an era when only three pairs
        # were listed, and a fold that ranks three instruments and picks the
        # better two is not a test of anything. Start instead at the first
        # moment --min-universe instruments each hold --min-train-bars, i.e.
        # the first date on which the rule could actually have been run.
        t1 = max(v[2] for v in spans.values())
        need = args.min_train_bars * period
        ready = sorted(v[1] + need for v in spans.values())
        if len(ready) < args.min_universe:
            print(f"[{period}s] only {len(ready)} stores, need {args.min_universe}")
            continue
        t0 = ready[args.min_universe - 1]
        if t0 >= t1:
            print(f"[{period}s] universe never reaches {args.min_universe} instruments")
            continue
        edges = [t0 + (t1 - t0) * k // args.folds for k in range(args.folds + 1)]
        # fold k: rank at edges[k], score (edges[k], edges[k+1]]

        for k in range(args.folds):
            train_end, test_end = edges[k], edges[k + 1]
            rows = []
            for sym, profile, path, datadir in stores:
                nbars, first_ts, last_ts = spans[sym]
                # Eligibility: enough calendar time before train_end to hold
                # min_train_bars at this period, and coverage of the test
                # window. Both are decided from the store's span alone, which
                # is information available at T.
                if first_ts > train_end - args.min_train_bars * period:
                    continue
                # Require most of the test window, not all of it: stores stop
                # on slightly different bars, and demanding exact coverage
                # silently emptied the final fold.
                if last_ts < test_end - (test_end - train_end) // 5:
                    continue
                r = measure_ratio(layout, sym, period, datadir, train_end, cache)
                if r["ratio"] is None or not math.isfinite(r["ratio"]):
                    continue
                f = measure_forward(layout, sym, profile, period, datadir,
                                    train_end, test_end, cache)
                if not f["ok"] or f["excess"] is None:
                    continue
                rows.append(dict(symbol=sym, profile=profile, period=period, fold=k,
                                 train_end=train_end, test_end=test_end,
                                 ratio=r["ratio"], alpha=r["alpha"],
                                 verdict=r["verdict"], train_candles=r["candles"],
                                 excess=f["excess"], sharpe=f["sharpe"],
                                 bench=f["bench"], trades=f["trades"], maxdd=f["maxdd"]))
                cache_path.write_text(json.dumps(cache))
            if len(rows) < 4:
                print(f"[{period}s fold {k+1}/{args.folds}] only {len(rows)} eligible, skipped")
                continue
            rows.sort(key=lambda r: r["ratio"])
            half = max(1, len(rows) // 2)
            for i, r in enumerate(rows):
                r["rank"] = i
                r["selected"] = i < half
                r["n_fold"] = len(rows)
            sel = [r["excess"] for r in rows if r["selected"]]
            rest = [r["excess"] for r in rows if not r["selected"]]
            print(f"[{period}s fold {k+1}/{args.folds}] n={len(rows)} "
                  f"selected {len(sel)} mean excess {sum(sel)/len(sel):+.3f} | "
                  f"rejected {len(rest)} mean {sum(rest)/len(rest):+.3f} | "
                  f"all {sum(r['excess'] for r in rows)/len(rows):+.3f}", flush=True)
            results.extend(rows)

    Path(args.results).parent.mkdir(parents=True, exist_ok=True)
    Path(args.results).write_text(json.dumps(results, indent=1))
    print(f"wrote {args.results} ({len(results)} rows)")


def cmd_report(args):
    rows = json.loads(Path(args.results).read_text())
    if not rows:
        raise SystemExit("no rows")
    by_tf = {}
    for r in rows:
        by_tf.setdefault(r["period"], []).append(r)

    print(f"{'TF':>5s} {'folds':>5s} {'n':>4s} {'selected':>9s} {'rejected':>9s} "
          f"{'all':>8s} {'edge':>7s} {'null p':>7s}")
    print("-" * 62)
    rng = random.Random(101)
    pooled = []
    for period in sorted(by_tf):
        rs = by_tf[period]
        folds = sorted({r["fold"] for r in rs})
        sel = [r["excess"] for r in rs if r["selected"]]
        rest = [r["excess"] for r in rs if not r["selected"]]
        allx = [r["excess"] for r in rs]
        edge = sum(sel) / len(sel) - sum(allx) / len(allx)

        # Random-selection null: redo every fold's pick at random, same size.
        hits, N = 0, 4000
        for _ in range(N):
            tot, cnt = 0.0, 0
            for f in folds:
                fr = [r for r in rs if r["fold"] == f]
                k = sum(1 for r in fr if r["selected"])
                for r in rng.sample(fr, k):
                    tot += r["excess"]; cnt += 1
            if cnt and (tot / cnt - sum(allx) / len(allx)) >= edge:
                hits += 1
        print(f"{gs.TF_NAME.get(period, period):>5s} {len(folds):5d} {len(rs):4d} "
              f"{sum(sel)/len(sel):+9.3f} {sum(rest)/len(rest):+9.3f} "
              f"{sum(allx)/len(allx):+8.3f} {edge:+7.3f} {hits/N:7.3f}")
        pooled.extend(rs)

    sel = [r["excess"] for r in pooled if r["selected"]]
    allx = [r["excess"] for r in pooled]
    rest = [r["excess"] for r in pooled if not r["selected"]]
    print("-" * 62)
    print(f"POOLED  selected {sum(sel)/len(sel):+.3f} (n={len(sel)})   "
          f"rejected {sum(rest)/len(rest):+.3f} (n={len(rest)})   "
          f"all {sum(allx)/len(allx):+.3f} (n={len(allx)})")
    print(f"        edge of the gate over trading everything: "
          f"{sum(sel)/len(sel) - sum(allx)/len(allx):+.3f} excess Sharpe")

    # Rank correlation, cluster-robust on instruments: does a better causal
    # rank predict a better forward outcome?
    rho, p, nclust = gs.cluster_spearman([r["ratio"] for r in pooled],
                                          [r["excess"] for r in pooled],
                                          [r["symbol"] for r in pooled])
    print(f"\nSpearman(causal ratio, forward excess) = {rho:+.3f}  "
          f"cluster-robust p = {p:.4f}  ({nclust} instruments)")
    print("  prediction: NEGATIVE (a lower ratio should forecast a better outcome)")
    win = sum(1 for r in pooled if r["selected"] and r["excess"] > 0)
    tot = sum(1 for r in pooled if r["selected"])
    win_all = sum(1 for r in pooled if r["excess"] > 0)
    print(f"\nPositive-excess rate: selected {win}/{tot} = {win/tot:.0%}   "
          f"universe {win_all}/{len(pooled)} = {win_all/len(pooled):.0%}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("run", "report"):
        p = sub.add_parser(name)
        p.add_argument("--results", default=str(DEFAULT_RESULTS))
        if name == "run":
            p.add_argument("--timeframes", type=int, nargs="+", default=[3600])
            p.add_argument("--folds", type=int, default=4)
            p.add_argument("--min-train-bars", type=int, default=4000,
                           help="bars an instrument needs before T to be ranked")
            p.add_argument("--min-universe", type=int, default=6,
                           help="instruments that must be eligible before the "
                                "walk-forward grid starts")
            p.add_argument("--cache", default=str(HERE / "results" / ".wf_cache.json"))
            p.add_argument("--repo"); p.add_argument("--data-dir")
            p.add_argument("--derived-dir"); p.add_argument("--fw-dir"); p.add_argument("--cli")
    args = ap.parse_args(argv)
    (cmd_run if args.cmd == "run" else cmd_report)(args)


if __name__ == "__main__":
    main()
