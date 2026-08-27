#!/usr/bin/env python3
"""Readout head-to-head: which reading of the spiral estimator's pencil modes
best predicts where tsmom beats buy-and-hold?

WHAT THIS TESTED
----------------
The spiral estimator returns, per contour radius, one complex pencil mode
omega.  The order alpha has to be READ OUT of that complex number, and the
model behind the contour (a logarithmic spiral of tightness sigma) implies

    omega = (sigma + i) * alpha

for a real order alpha.  Three readouts of the same modes:

    re    alpha = Re(omega) / sigma                    (what shipped before)
    im    alpha = Im(omega)                            (secondary candidate)
    proj  alpha = (sigma*Re + Im) / (1 + sigma^2)      (pre-registered candidate:
                                                        the least-squares reading
                                                        the model implies)

and the residual off-manifold part

    beta  = (sigma*Im - Re) / (1 + sigma^2)

which measures how far the mode sits OFF the real-order manifold - a sanity
check, not a predictor: |beta| near zero means the fitted order really is real.

The re-scoring runs the spiral_modes helper (experiments/tools/) on every store
the sweep measured.  That helper replicates cmdOrderSpectrum's construction
exactly and emits the RAW modes, so all readouts come from one identical run
and the `re` readout doubles as a cross-check that reproduces the sweep's own
parsed numbers (reported as xcheck_dev / max deviation).

For each readout the identification statistic mirrors the sweep verdict:

    ratio = tail spread of alpha over the 3 smallest radii
            / max tail spread of the two power-law references (k^-0.3, k^-0.6)
              read out the SAME way

PRE-REGISTERED COMPARISON
-------------------------
Against the backtests already in results/gate_results.json, on the same row set
the sweep uses for statistics (>=8 trades, fw rows and the duplicate full-depth
4h rows excluded): the readout with the larger |Spearman(ratio, excess Sharpe)|
carrying the predicted NEGATIVE sign wins.

Outcome: proj -0.263 (perm p 0.031) beat re -0.231 (p 0.059), which is why
src/math/spiral.cpp now ships the projection readout.

HOW TO RE-RUN
-------------
    cmake -S <repo> -B <repo>/build && cmake --build <repo>/build
    python3 experiments/order_gate/gate_sweep.py run     # must exist first
    python3 experiments/order_gate/im_rescore.py run     # writes results/im_results.json
    python3 experiments/order_gate/im_rescore.py report

Store paths are resolved from the manifest in gate_sweep.py, not from the
absolute directory recorded in the results file, so an older results JSON
re-scores fine after the data has moved.  `run` overwrites
results/im_results.json - the committed copy is the record of the
pre-registered run, so write elsewhere with --results if you are exploring.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gate_sweep as gs  # noqa: E402  (same directory; keeps the manifest single-sourced)

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
DEFAULT_OUT = HERE / "results" / "im_results.json"
SIGMA = 0.3


def find_spiral_modes(repo, build_dir=None, override=None):
    """Locate the spiral_modes helper built from experiments/tools/."""
    if override:
        p = Path(override).resolve()
        if not p.is_file():
            sys.exit(f"spiral_modes: no such file {p}")
        return p
    build = Path(build_dir).resolve() if build_dir else Path(repo) / "build"
    probed = [build / "experiments" / "tools" / "spiral_modes", build / "spiral_modes"]
    for cand in probed:
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand
    sys.exit("tool 'spiral_modes' not found. Probed:\n"
             + "".join(f"  {p}\n" for p in probed)
             + f"Build the experiment tools first, e.g.\n"
               f"  cmake -S {repo} -B {build} && cmake --build {build}\n"
               f"or point at it with --spiral-modes / --build-dir.")


def readouts(mode):
    re_, im_ = mode
    return {
        "re": re_ / SIGMA,
        "im": im_,
        "proj": (SIGMA * re_ + im_) / (1.0 + SIGMA * SIGMA),
        "beta": (SIGMA * im_ - re_) / (1.0 + SIGMA * SIGMA),
    }


def tail_stats(modes, key):
    vals = [readouts(m)[key] for m in modes if m is not None]
    if len(vals) < 2:
        return None, None
    tail = vals[-3:] if len(vals) >= 3 else vals
    return max(tail) - min(tail), sum(tail) / len(tail)


def score_store(tool, path):
    p = subprocess.run([str(tool), str(path)], capture_output=True, text=True)
    if p.returncode != 0:
        return None
    data = json.loads(p.stdout)
    out = dict(candles=data["candles"], lags=data["lags"])
    for key in ("re", "im", "proj", "beta"):
        sprd, mean = tail_stats(data["modes"]["measured"], key)
        ref = max(filter(None, [tail_stats(data["modes"][r], key)[0] for r in ("pl03", "pl06")]),
                  default=None)
        out[f"{key}_tail_spread"] = sprd
        out[f"{key}_tail_mean"] = mean
        if key != "beta":
            out[f"{key}_ref"] = ref
            out[f"{key}_ratio"] = (sprd / ref) if (sprd is not None and ref) else None
    return out


def resolve_store(layout, row):
    """Store path for a sweep row under the CURRENT layout.

    The `datadir` recorded in a results file is an absolute path from the run
    that produced it, so it is ignored here: the (profile, period, tag) rung
    tells us which store source the row belongs to, and the layout tells us
    where that source lives now."""
    kind = row.get("store_kind") or gs.store_kind(row["profile"], row["period"], row.get("tag", ""))
    return gs.store_path(layout.dir_for(kind), row["symbol"], row["period"])


def cmd_run(args):
    layout = gs.layout_from_args(args)
    tool = find_spiral_modes(layout.repo, args.build_dir, args.spiral_modes)
    out_path = Path(args.results).resolve()
    with open(args.gate_results) as f:
        sweep = json.load(f)
    print(f"{len(sweep)} sweep rows from {args.gate_results}\nspiral_modes: {tool}\n")
    results = []
    for i, row in enumerate(sweep):
        path = resolve_store(layout, row)
        rec = dict(label=row["label"], symbol=row["symbol"], period=row["period"],
                   tag=row.get("tag", ""), profile=row["profile"],
                   excess_sharpe=row.get("excess_sharpe"), trades=row.get("trades", 0),
                   sweep_ratio=row.get("ratio"), sweep_tail=row.get("tail_spread"))
        sc = score_store(tool, path)
        if sc:
            rec.update(sc)
            # Cross-check: the re-readout must reproduce the sweep's numbers.
            if rec.get("sweep_tail") is not None and sc.get("re_tail_spread") is not None:
                rec["xcheck_dev"] = abs(sc["re_tail_spread"] - rec["sweep_tail"])
        else:
            print(f"  ! {rec['label']}: spiral_modes failed on {path}", file=sys.stderr)
        results.append(rec)
        print(f"[{i+1}/{len(sweep)}] {rec['label']:20s} "
              f"re_ratio={rec.get('re_ratio') or float('nan'):6.2f} "
              f"proj_ratio={rec.get('proj_ratio') or float('nan'):6.2f} "
              f"im_ratio={rec.get('im_ratio') or float('nan'):6.2f} "
              f"xdev={rec.get('xcheck_dev', float('nan')):.3f}", flush=True)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w") as f:
        json.dump(results, f, indent=1)
    print(f"wrote {out_path}")


def cmd_report(args):
    min_trades = args.min_trades
    with open(args.results) as f:
        rows = json.load(f)
    usable = [r for r in rows
              if r.get("excess_sharpe") is not None and r.get("trades", 0) >= min_trades
              and r.get("tag") != "fw" and not (r.get("tag") == "full" and r["period"] == 14400)
              and all(r.get(f"{k}_ratio") is not None for k in ("re", "im", "proj"))]
    xdevs = [r["xcheck_dev"] for r in usable if r.get("xcheck_dev") is not None]
    print(f"n={len(usable)} rows (>= {min_trades} trades); "
          f"re-readout cross-check max deviation vs sweep: {max(xdevs):.4f}" if xdevs else "no xcheck")
    ys = [r["excess_sharpe"] for r in usable]
    print(f"\n{'readout':8s} {'Spearman':>9s} {'perm p':>8s}   (prediction: negative)")
    for key in ("re", "proj", "im"):
        xs = [r[f"{key}_ratio"] for r in usable]
        rho, p = gs.spearman(xs, ys)
        print(f"{key:8s} {rho:+9.3f} {p:8.4f}")
    for name, sel in [("crypto", lambda r: r["profile"] == "crypto"),
                      ("equity", lambda r: r["profile"] == "equity")]:
        sub = [r for r in usable if sel(r)]
        if len(sub) < 5:
            continue
        ys2 = [r["excess_sharpe"] for r in sub]
        line = f"  {name} (n={len(sub)}):"
        for key in ("re", "proj", "im"):
            rho, p = gs.spearman([r[f"{key}_ratio"] for r in sub], ys2)
            line += f"  {key} {rho:+.3f} (p={p:.3f})"
        print(line)
    # Beta sanity: measured series should sit near zero when memory is real.
    print("\nbeta tail means, most-identified rows first (|beta| small = real order):")
    for r in sorted(usable, key=lambda r: r["proj_ratio"])[:10]:
        print(f"  {r['label']:20s} proj_ratio={r['proj_ratio']:6.2f} "
              f"beta={r.get('beta_tail_mean', float('nan')):+.3f} "
              f"excess={r['excess_sharpe']:+.2f}")


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="im_rescore.py",
        description="Re-score every sweep store's pencil modes under three "
                    "readouts of the order (re / im / proj) and test which "
                    "identification ratio best rank-predicts tsmom excess "
                    "Sharpe.",
        epilog="Pre-registered winner: proj, Spearman -0.263 (perm p 0.031) vs "
               "re -0.231 (p 0.059). Reuses gate_sweep.py's manifest and "
               "permutation test; see its --help for the store layout.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True, metavar="{run,report}")

    p_run = sub.add_parser("run", help="run spiral_modes on every sweep store, write results JSON")
    gs.add_layout_args(p_run)
    p_run.add_argument("--build-dir", metavar="DIR",
                       help="CMake build tree holding spiral_modes (default: <repo>/build)")
    p_run.add_argument("--spiral-modes", metavar="BIN",
                       help="override the spiral_modes binary path")
    p_run.add_argument("--gate-results", default=str(gs.DEFAULT_RESULTS), metavar="JSON",
                       help=f"sweep results to re-score (default: {gs.DEFAULT_RESULTS})")
    p_run.add_argument("--results", default=str(DEFAULT_OUT), metavar="JSON",
                       help=f"output file (default: {DEFAULT_OUT}) - OVERWRITES the recorded run")
    p_run.set_defaults(func=cmd_run)

    p_rep = sub.add_parser("report", help="head-to-head comparison from an existing results JSON")
    p_rep.add_argument("--results", default=str(DEFAULT_OUT), metavar="JSON",
                       help=f"input file (default: {DEFAULT_OUT})")
    p_rep.add_argument("--min-trades", type=int, default=8, metavar="N",
                       help="drop rows with fewer trades (default: 8, the pre-registered threshold)")
    p_rep.set_defaults(func=cmd_report)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    main()
