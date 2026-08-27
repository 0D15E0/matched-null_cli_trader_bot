#!/usr/bin/env python3
"""Build the derived candle stores the order-gate sweep measures on.

WHY DERIVED STORES EXIST
------------------------
The sweep (gate_sweep.py) asks whether the spiral estimator's identification
ratio predicts tsmom excess Sharpe across timeframes.  If each timeframe were
fetched separately from the venue, coarse timeframes would cover a longer
history than fine ones, and "timeframe" would be confounded with "which years
of market you looked at".  So for each instrument the mid ladder is RESAMPLED
from one deep store, making every rung of that instrument's ladder cover an
identical window:

  CRYPTO   data/<PAIR>_900.ctc    ->  data/derived/<PAIR>_{1800,3600,7200,14400}.ctc
           data/<PAIR>_14400.ctc  ->  data/derived/<PAIR>_86400.ctc
             (the direct 4h fetch USUALLY reaches further back than 15m history
              does, so it and the 1d resampled from it are kept as "full"-depth
              rungs -- but NOT always: ETH_USDT's pulled 4h store starts
              2016-05 because its earliest bars had to be skipped around a
              corrupt Dec-2015 window, while its 15m-derived 4h reaches
              further. Do not assume "full" means deeper for every pair.)

  EQUITY   data/<SYM>_3600.ctc    ->  data/derived/<SYM>_{7200,14400}.ctc
           data/<SYM>_86400.ctc   ->  data/derived/fw/<SYM>_86400.ctc
             (fw = "fixed window": the 10-year daily series truncated to start
              at the 1h store's first bar, so the 1d rung can be compared
              against the intraday ladder on the same window)

Pulled stores in data/ are inputs and are never written here.

TOOLS
-----
Resampling and truncation are done by the C++ helpers in experiments/tools/ so
they go through the same CandleStore reader/writer (and its spacing validation)
as the rest of the repo:

  resample  <in.ctc> <out.ctc> <targetPeriodSeconds>
  keeplast  <in.ctc> <out.ctc> <N|epochSeconds>
  storeinfo <store.ctc>   ->  "<symbol> <period> <n> <first_ts> <last_ts>"

They are probed at <repo>/build/experiments/tools/<name> and <repo>/build/<name>
(CMake's layout depends on how the tools subdirectory is wired up).

HOW TO RE-RUN
-------------
    cmake -S <repo> -B <repo>/build && cmake --build <repo>/build
    python3 experiments/order_gate/build_stores.py --dry-run   # show the plan
    python3 experiments/order_gate/build_stores.py             # write the stores

Idempotent: re-running just overwrites the derived stores from the pulled ones.
All paths default off this file's location, so any working directory works.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]

CRYPTO = ["BTC_USDT", "ETH_USDT", "XRP_USDT", "LTC_USDT", "DOGE_USDT", "ADA_USDT", "TRX_USDT", "SOL_USDT"]
EQUITY = ["AAPL", "AMZN", "META", "TSLA", "ASML.AS"]

CRYPTO_FROM_900 = (1800, 3600, 7200, 14400)
EQUITY_FROM_3600 = (7200, 14400)


def find_tool(name, repo, build_dir=None, override=None):
    """Locate a helper built from experiments/tools/.  CMake may place it in a
    subdirectory mirroring the source tree or straight in the build root."""
    if override:
        p = Path(override).resolve()
        if not p.is_file():
            sys.exit(f"{name}: no such file {p}")
        return p
    build = Path(build_dir).resolve() if build_dir else repo / "build"
    probed = [build / "experiments" / "tools" / name, build / name]
    for cand in probed:
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand
    sys.exit(f"tool '{name}' not found. Probed:\n"
             + "".join(f"  {p}\n" for p in probed)
             + f"Build the experiment tools first, e.g.\n"
               f"  cmake -S {repo} -B {build} && cmake --build {build}\n"
               f"or point at it with --{name} / --build-dir.")


class Tools:
    def __init__(self, args, repo):
        self.resample = find_tool("resample", repo, args.build_dir, args.resample)
        self.keeplast = find_tool("keeplast", repo, args.build_dir, args.keeplast)
        self.storeinfo = find_tool("storeinfo", repo, args.build_dir, args.storeinfo)


def run(args, dry_run=False):
    args = [str(a) for a in args]
    if dry_run:
        print("  would run: " + " ".join(args))
        return True
    p = subprocess.run(args, capture_output=True, text=True)
    if p.returncode != 0:
        print(f"FAIL: {' '.join(args)}\n{p.stdout}{p.stderr}", file=sys.stderr)
        return False
    return True


def first_ts(storeinfo, path):
    """First bar timestamp of a store, or None if it cannot be read."""
    p = subprocess.run([str(storeinfo), str(path)], capture_output=True, text=True)
    if p.returncode != 0:
        return None
    toks = p.stdout.split()
    if len(toks) < 5:
        return None
    return int(toks[-2])  # <symbol> <period> <n> <first_ts> <last_ts>


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="build_stores.py",
        description="Resample pulled venue stores into the derived stores the "
                    "order-gate sweep measures on: the crypto 30m/1h/2h/4h "
                    "ladder from each pair's 15m store, crypto 1d from the "
                    "direct 4h fetch, equity 2h/4h from each symbol's 1h "
                    "store, and an equity 1d truncated to the 1h window.",
        epilog="Reads only from --data-dir; writes only into --derived-dir and "
               "--fw-dir. Safe to re-run. See gate_sweep.py for the manifest "
               "that consumes these files.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", metavar="DIR",
                    help=f"repo root (default: two levels above this script, {REPO_ROOT})")
    ap.add_argument("--data-dir", metavar="DIR",
                    help="pulled venue stores to read (default: <repo>/data)")
    ap.add_argument("--derived-dir", metavar="DIR",
                    help="where resamples are written (default: <data-dir>/derived)")
    ap.add_argument("--fw-dir", metavar="DIR",
                    help="where matched-window truncations are written (default: <derived-dir>/fw)")
    ap.add_argument("--build-dir", metavar="DIR",
                    help="CMake build tree holding the tools (default: <repo>/build)")
    ap.add_argument("--resample", metavar="BIN", help="override the resample binary path")
    ap.add_argument("--keeplast", metavar="BIN", help="override the keeplast binary path")
    ap.add_argument("--storeinfo", metavar="BIN", help="override the storeinfo binary path")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the commands that would run, write nothing")
    args = ap.parse_args(argv)

    repo = Path(args.repo).resolve() if args.repo else REPO_ROOT
    data = Path(args.data_dir).resolve() if args.data_dir else repo / "data"
    derived = Path(args.derived_dir).resolve() if args.derived_dir else data / "derived"
    fw = Path(args.fw_dir).resolve() if args.fw_dir else derived / "fw"
    tools = Tools(args, repo)

    print(f"repo     {repo}")
    print(f"data     {data}   (read)")
    print(f"derived  {derived} (write)")
    print(f"fw       {fw} (write)")
    print(f"tools    {tools.resample.parent}\n")
    if not data.is_dir():
        sys.exit(f"pulled store directory not found: {data}")
    if not args.dry_run:
        derived.mkdir(parents=True, exist_ok=True)
        fw.mkdir(parents=True, exist_ok=True)

    made = failed = 0

    # Crypto: the 30m/1h/2h/4h ladder all derives from the deep 15m store, so
    # every rung of a pair's ladder covers the IDENTICAL window (no TF-epoch
    # confound). The direct 4h fetch is kept as the "full"-depth rung, which
    # for most pairs reaches further back than 15m history does -- but not for
    # ETH_USDT, whose pulled 4h had to start after a corrupt Dec-2015 window
    # (bars with low=0 that CandleStore rightly refuses). Verify with storeinfo
    # rather than assuming.
    for sym in CRYPTO:
        p900 = data / f"{sym}_900.ctc"
        p14400 = data / f"{sym}_14400.ctc"
        if p900.exists():
            for per in CRYPTO_FROM_900:
                ok = run([tools.resample, p900, derived / f"{sym}_{per}.ctc", per], args.dry_run)
                made, failed = made + ok, failed + (not ok)
        else:
            print(f"skip {sym}: no {p900.name} in {data}")
        if p14400.exists():
            ok = run([tools.resample, p14400, derived / f"{sym}_86400.ctc", 86400], args.dry_run)
            made, failed = made + ok, failed + (not ok)
        else:
            print(f"skip {sym}: no {p14400.name} in {data}")

    # Equities: 5m/15m/1h are pulled (phase-filtered at fetch time); 2h/4h
    # derive from the 1h store, and the pulled 10y daily gets an fw twin
    # truncated to the 1h store's first bar.
    for sym in EQUITY:
        p3600 = data / f"{sym}_3600.ctc"
        if not p3600.exists():
            print(f"skip {sym}: no {p3600.name} in {data}")
            continue
        for per in EQUITY_FROM_3600:
            ok = run([tools.resample, p3600, derived / f"{sym}_{per}.ctc", per], args.dry_run)
            made, failed = made + ok, failed + (not ok)
        daily = data / f"{sym}_86400.ctc"
        if not daily.exists():
            print(f"skip {sym} fw: no {daily.name} in {data}")
            continue
        if args.dry_run:
            print(f"  would read first bar timestamp of {p3600} via {tools.storeinfo.name}")
            run([tools.keeplast, daily, fw / f"{sym}_86400.ctc", "<first_ts of 1h store>"], True)
            continue
        ts = first_ts(tools.storeinfo, p3600)
        if not ts:
            print(f"skip {sym} fw: cannot read first timestamp of {p3600}", file=sys.stderr)
            failed += 1
            continue
        ok = run([tools.keeplast, daily, fw / f"{sym}_86400.ctc", ts])
        made, failed = made + ok, failed + (not ok)

    if args.dry_run:
        print("\ndry run: nothing written")
        return 0
    print("\nderived stores:")
    for d in (derived, fw):
        names = sorted(f for f in os.listdir(d) if f.endswith(".ctc")) if d.is_dir() else []
        print(f"  {d}: {len(names)} stores")
    if failed:
        print(f"{failed} step(s) failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
