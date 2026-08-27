#!/usr/bin/env python3
"""Order-gate sweep: does the spiral estimator's identification statistic
predict where trend-following beats buy-and-hold?

WHAT THIS TESTED
----------------
`cli_trader order-spectrum` estimates the ORDER of a series' volatility memory
with a logarithmic-spiral contour + matrix-pencil estimator (src/math/spiral.*,
src/math/pencil.*).  It reports, for each of five contour radii, the estimated
order alpha; the estimator is judged to have IDENTIFIED an order when alpha
stops moving as the contour shrinks.  The statistic used here is

    ratio = tail spread of alpha over the 3 smallest radii
            ------------------------------------------------
            max tail spread of the two power-law reference series (k^-0.3, k^-0.6)

i.e. how unstable the measured series' order is, in units of how unstable a
KNOWN fractional-order series looks under the same lag budget.  Lower ratio =
the order is resolvable = the series has memory the estimator can pin down.

The pre-registered hypothesis: a resolvable memory order marks instruments and
timeframes where the tsmom trend strategy earns its risk.  So the prediction is
a NEGATIVE rank correlation between `ratio` and tsmom excess Sharpe (strategy
Sharpe minus buy-and-hold Sharpe) across (instrument, timeframe) stores.

For each store in the manifest the sweep runs two commands and parses them:
  1. cli_trader order-spectrum ... -> candles, lags, tail spread, alpha, verdict
  2. cli_trader backtest --strategy tsmom --vol-target 0.20 ... -> Sharpe,
     benchmark Sharpe, excess Sharpe, trades, span, max drawdown
then rank-correlates ratio against excess Sharpe with a permutation test.

Result of the recorded run (results/gate_results.json, 98 stores): Spearman
-0.263 under the projection readout that src/math/spiral.cpp now ships.

READ THE P-VALUE CAVEAT.  The row-level permutation test below reports p =
0.031 for that correlation, and that number is TOO SMALL.  It permutes rows as
if all 68 were independent, but they come from only 13 instruments and rows
sharing an instrument share a price series.  `report` therefore also prints a
CLUSTER-ROBUST permutation (whole instrument blocks resampled), under which the
raw correlation is p = 0.103 -- not significant.  What does survive clustering:
controlling for sample size lifts the correlation to -0.376 at p = 0.017, and
the within-timeframe decomposition gives -0.317 at p = 0.009, negative in 7 of
7 timeframes, while the within-instrument decomposition is exactly 0.000.  The
gate ranks INSTRUMENTS, not timeframes.  See im_rescore.py for the readout
head-to-head and experiments/README.md for the writeup.

THE MANIFEST (which store each row is measured on)
--------------------------------------------------
This is load-bearing: the manifest records which file on disk each row was
measured on, and changing it silently changes the experiment.  Three store
sources, all under <repo>/data:

  pulled   data/            fetched straight from the venue
  derived  data/derived/    resampled from a pulled store by build_stores.py
  fw       data/derived/fw/ a pulled store truncated to another store's window

  CRYPTO (BTC/ETH/XRP/LTC/DOGE/ADA/TRX/SOL, all _USDT)
    5m   300           pulled    own ~2y window
    15m  900           pulled    full venue depth
    30m  1800          derived   resampled from that pair's 900 store
    1h   3600          derived   resampled from that pair's 900 store
    2h   7200          derived   resampled from that pair's 900 store
    4h   14400         derived   resampled from that pair's 900 store
    4h   14400 "full"  pulled    the direct 4h fetch (usually deeper than the
                                 15m-derived rung, but not for ETH_USDT)
    1d   86400 "full"  derived   resampled from the pulled 14400 store

    Deriving 30m..4h from the one 15m store makes a pair's whole ladder cover
    an IDENTICAL window, so timeframe is not confounded with sample epoch.
    The "full" rows are kept for the table only, not the statistics.

  EQUITY (AAPL/AMZN/META/TSLA/ASML.AS)
    5m   300           pulled
    15m  900           pulled
    1h   3600          pulled
    2h   7200          derived   resampled from that symbol's 3600 store
    4h   14400         derived   resampled from that symbol's 3600 store
    1d   86400         pulled    the 10-year daily series
    1d   86400 "fw"    fw        the daily series truncated to the 3600 window

WHICH ROWS ENTER THE STATISTICS
-------------------------------
The table prints everything; the correlations use one row per (pair, TF):
  * fw rows are excluded (they duplicate a 1d row over a shorter window),
  * the 14400 "full" rows are excluded (they duplicate the derived 4h rows),
  * the 86400 "full" rows are kept (they have no untagged twin),
  * rows with fewer than 8 trades are excluded (Sharpe is meaningless there).
These filters are part of the pre-registration - do not tune them.

HOW TO RE-RUN
-------------
    cmake -S <repo> -B <repo>/build && cmake --build <repo>/build   # cli_trader + tools
    python3 experiments/order_gate/build_stores.py                  # data/derived/**
    python3 experiments/order_gate/gate_sweep.py run                # ~30 min
    python3 experiments/order_gate/gate_sweep.py report

`run` overwrites results/gate_results.json - the committed copy is the record of
the pre-registered run, so write elsewhere with --results if you are exploring.
All paths default off this file's location, so any working directory works.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
DEFAULT_RESULTS = HERE / "results" / "gate_results.json"

CRYPTO = ["BTC_USDT", "ETH_USDT", "XRP_USDT", "LTC_USDT", "DOGE_USDT", "ADA_USDT", "TRX_USDT", "SOL_USDT"]
EQUITY = ["AAPL", "AMZN", "META", "TSLA", "ASML.AS"]
TF_NAME = {300: "5m", 900: "15m", 1800: "30m", 3600: "1h", 7200: "2h", 14400: "4h", 86400: "1d"}

# (period, store kind, tag) rungs, in manifest order. See the module docstring.
LADDER = {
    "crypto": [
        (300, "pulled", ""),
        (900, "pulled", ""),
        (1800, "derived", ""),
        (3600, "derived", ""),
        (7200, "derived", ""),
        (14400, "derived", ""),
        (14400, "pulled", "full"),
        (86400, "derived", "full"),
    ],
    "equity": [
        (300, "pulled", ""),
        (900, "pulled", ""),
        (3600, "pulled", ""),
        (7200, "derived", ""),
        (14400, "derived", ""),
        (86400, "pulled", ""),
        (86400, "fw", "fw"),
    ],
}
SYMBOLS = {"crypto": CRYPTO, "equity": EQUITY}


class Layout:
    """Where the repo keeps its stores and binaries.  Everything is derived
    from the repo root, which is derived from this file's location."""

    def __init__(self, repo=None, data=None, derived=None, fw=None, cli=None):
        self.repo = Path(repo).resolve() if repo else REPO_ROOT
        self.data = Path(data).resolve() if data else self.repo / "data"
        self.derived = Path(derived).resolve() if derived else self.data / "derived"
        self.fw = Path(fw).resolve() if fw else self.derived / "fw"
        self.cli = Path(cli).resolve() if cli else self.repo / "build" / "cli_trader"

    def dir_for(self, kind):
        try:
            return {"pulled": self.data, "derived": self.derived, "fw": self.fw}[kind]
        except KeyError:
            raise ValueError(f"unknown store kind {kind!r}") from None

    def describe(self):
        return (f"repo    {self.repo}\ndata    {self.data}\nderived {self.derived}\n"
                f"fw      {self.fw}\ncli     {self.cli}")


def store_path(datadir, symbol, period):
    """Path of one candle store.  `datadir` is a directory, not a store kind."""
    return os.path.join(str(datadir), f"{symbol}_{period}.ctc")


def profile_of(symbol):
    return "crypto" if symbol in CRYPTO else "equity"


def store_kind(profile, period, tag):
    """Which store source a manifest row was measured on.  Single source of
    truth for the mapping - im_rescore.py resolves paths through this too, so
    an old results file with stale absolute datadirs still relocates cleanly."""
    tag = tag or ""
    for per, kind, t in LADDER[profile]:
        if per == period and t == tag:
            return kind
    raise KeyError(f"no manifest rung for profile={profile} period={period} tag={tag!r}")


def build_manifest(layout, require_exists=True):
    rows = []
    for profile, symbols in SYMBOLS.items():
        for sym in symbols:
            for period, kind, tag in LADDER[profile]:
                datadir = layout.dir_for(kind)
                rows.append(dict(symbol=sym, period=period, datadir=str(datadir),
                                 store_kind=kind, tag=tag, profile=profile,
                                 label=f"{sym}@{TF_NAME[period]}" + (f"~{tag}" if tag else "")))
    if require_exists:
        rows = [r for r in rows if os.path.exists(store_path(r["datadir"], r["symbol"], r["period"]))]
    return rows


def run_cmd(layout, args, timeout=600):
    p = subprocess.run([str(a) for a in args], capture_output=True, text=True,
                       timeout=timeout, cwd=str(layout.repo))
    return p.stdout + p.stderr


# Trailing summary columns of the order-spectrum table, in print order. This
# list is the parser's contract with src/main.cpp's printSpectrumRow.
#
# It is written out explicitly, and the header is parsed to check it, because
# the alternative bit us: the parser used to index from the END of the row
# (toks[-2], toks[-1]) and the command later grew a tailBeta column. Nothing
# failed - the parser silently started reading tailMean as the tail spread and
# tailBeta as alpha, producing a full results file of confident nonsense. An
# index-from-the-end parser cannot notice a new column; a named one can.
SPECTRUM_TAIL_COLUMNS = ["spread", "tailSprd", "tailMean", "tailBeta", "betaSprd"]


def _tail_columns(out):
    """Column names from the printed header, so a schema change is detectable."""
    for line in out.splitlines():
        toks = line.split()
        if toks[:1] == ["series"] and "tailSprd" in toks:
            # everything after the r=<radius> columns
            return [t for t in toks[1:] if not t.startswith("r=")]
    return None


def parse_spectrum(out, symbol):
    res = dict(spec_ok=False)
    m = re.search(r"Samples:\s+(\d+) candles, autocorrelation to lag (\d+)", out)
    if m:
        res["candles"] = int(m.group(1))
        res["lags"] = int(m.group(2))

    cols = _tail_columns(out)
    if cols is None:
        res["parse_error"] = "no spectrum header found in output"
        res["verdict"] = "NOCONV"
        return res
    if cols != SPECTRUM_TAIL_COLUMNS:
        # Refuse to guess. Reading the wrong column is worse than not reading.
        res["parse_error"] = ("order-spectrum column layout changed: expected %s, saw %s. "
                              "Update SPECTRUM_TAIL_COLUMNS to match src/main.cpp."
                              % (SPECTRUM_TAIL_COLUMNS, cols))
        res["verdict"] = "NOCONV"
        return res

    ntail = len(cols)
    idx = {name: -ntail + i for i, name in enumerate(cols)}

    def cell(toks, name):
        v = toks[idx[name]]
        return None if v == "n/a" else float(v)

    ref_tails = []
    for line in out.splitlines():
        toks = line.split()
        if len(toks) < ntail + 1:
            continue
        if toks[0] == symbol:
            sprd = cell(toks, "tailSprd")
            if sprd is not None:
                res["tail_spread"] = sprd
                res["alpha"] = cell(toks, "tailMean")
                res["beta"] = cell(toks, "tailBeta")
                res["beta_spread"] = cell(toks, "betaSprd")
        elif line.strip().startswith("power law"):
            sprd = cell(toks, "tailSprd")
            if sprd is not None:
                ref_tails.append(sprd)

    if ref_tails and "tail_spread" in res:
        res["ref_tail"] = max(ref_tails)  # same statistic the verdict uses
        res["ratio"] = res["tail_spread"] / res["ref_tail"] if res["ref_tail"] > 0 else float("nan")
        res["spec_ok"] = True

    # Verdict strings, in the order src/main.cpp emits them. COMPLEX must be
    # tested before NOT: a rejected-as-unstable complex order prints a NOT
    # identified line too, and the distinction matters for the record.
    if "FRACTIONAL order identified" in out:
        res["verdict"] = "FRACTIONAL"
    elif "COMPLEX order:" in out:
        res["verdict"] = "COMPLEX"
    elif "INTEGER order" in out:
        res["verdict"] = "INTEGER"
    elif "NOT identified" in out:
        res["verdict"] = "NOT"
    else:
        res["verdict"] = "NOCONV"
    return res


def parse_backtest(out):
    res = dict(bt_ok=False)
    m = re.search(r"Span:\s+([\d.]+) years, ([\d.]+) bars/year", out)
    if m:
        res["years"] = float(m.group(1))
        res["bars_per_year"] = float(m.group(2))
    m = re.search(r"Sharpe \(ann\.\):\s+(-?[\d.]+) \+/- ([\d.]+)\s+(-?[\d.]+)\s+(-?[\d.]+)", out)
    if m:
        res["sharpe"] = float(m.group(1))
        res["sharpe_se"] = float(m.group(2))
        res["bench_sharpe"] = float(m.group(3))
        res["excess_sharpe"] = float(m.group(4))
        res["bt_ok"] = True
    m = re.search(r"Max drawdown:\s+([\d.]+)%\s+([\d.]+)%", out)
    if m:
        res["maxdd"] = float(m.group(1))
        res["bench_maxdd"] = float(m.group(2))
    m = re.search(r"Num trades:\s+(\d+)", out)
    if m:
        res["trades"] = int(m.group(1))
    return res


def cmd_manifest(args):
    layout = layout_from_args(args)
    print(layout.describe(), "\n")
    rows = build_manifest(layout, require_exists=False)
    missing = 0
    missing_kinds = set()
    print(f"{'pair@TF':20s} {'kind':8s} {'store'}")
    for r in rows:
        path = store_path(r["datadir"], r["symbol"], r["period"])
        here = os.path.exists(path)
        missing += 0 if here else 1
        if not here:
            missing_kinds.add(r["store_kind"])
        try:
            shown = os.path.relpath(path, layout.repo)
        except ValueError:
            shown = path
        print(f"{r['label']:20s} {r['store_kind']:8s} {shown}{'' if here else '   [MISSING]'}")
    print(f"\n{len(rows)} manifest rows, {len(rows) - missing} present, {missing} missing")
    # Only point at build_stores.py for stores it can actually produce. A missing
    # PULLED store is a fetch gap (e.g. DOGE_USDT_300, which was never acquired);
    # re-running the regeneration for one is a guaranteed no-op.
    if missing_kinds & {"derived", "fw"}:
        print("missing derived stores? run build_stores.py first")
    if "pulled" in missing_kinds:
        print("missing PULLED stores are a fetch gap - build_stores.py cannot create "
              "them; the sweep will simply run without those rows")


def cmd_run(args):
    layout = layout_from_args(args)
    if not layout.cli.exists():
        sys.exit(f"cli_trader not found at {layout.cli}\n"
                 f"build it first (cmake --build {layout.repo / 'build'}) or pass --cli")
    manifest = build_manifest(layout)
    results_path = Path(args.results).resolve()
    print(f"{len(manifest)} (instrument, timeframe) stores found")
    results = []
    for i, row in enumerate(manifest):
        sym, per, dd = row["symbol"], row["period"], row["datadir"]
        label = row["label"]
        spec_out = run_cmd(layout, [layout.cli, "order-spectrum", "--symbol", sym,
                                    "--period", str(per), "--data-dir", dd])
        spec = parse_spectrum(spec_out, sym)
        bt_out = run_cmd(layout, [layout.cli, "backtest", "--symbol", sym, "--period", str(per),
                                  "--strategy", "tsmom", "--vol-target", "0.20",
                                  "--profile", row["profile"], "--data-dir", dd])
        bt = parse_backtest(bt_out)
        rec = dict(row, **spec, **bt)
        results.append(rec)
        ex = rec.get("excess_sharpe")
        print(f"[{i+1}/{len(manifest)}] {label:18s} verdict={rec['verdict']:10s} "
              f"ratio={rec.get('ratio', float('nan')):6.2f} "
              f"excess={'n/a' if ex is None else f'{ex:+.2f}'} "
              f"trades={rec.get('trades', 0)}", flush=True)
    results_path.parent.mkdir(parents=True, exist_ok=True)
    with open(results_path, "w") as f:
        json.dump(results, f, indent=1)
    print(f"wrote {results_path}")


def _ranks(v):
    order = sorted(range(len(v)), key=lambda i: v[i])
    r = [0.0] * len(v)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and v[order[j + 1]] == v[order[i]]:
            j += 1
        avg = (i + j) / 2.0 + 1.0
        for k in range(i, j + 1):
            r[order[k]] = avg
        i = j + 1
    return r


def _pearson(a, b):
    n = len(a)
    if n < 2:
        return float("nan")
    ma, mb = sum(a) / n, sum(b) / n
    num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
    da = math.sqrt(sum((x - ma) ** 2 for x in a))
    db = math.sqrt(sum((y - mb) ** 2 for y in b))
    return num / (da * db) if da > 0 and db > 0 else float("nan")


def _residualize(y, regressors):
    """Least-squares residuals of y on [1, *regressors].  Plain normal
    equations with partial pivoting - the design is tiny and well conditioned."""
    n = len(y)
    X = [[1.0] + [reg[i] for reg in regressors] for i in range(n)]
    p = len(X[0])
    XtX = [[sum(X[i][a] * X[i][b] for i in range(n)) for b in range(p)] for a in range(p)]
    Xty = [sum(X[i][a] * y[i] for i in range(n)) for a in range(p)]
    M = [row[:] + [Xty[k]] for k, row in enumerate(XtX)]
    for c in range(p):
        piv = max(range(c, p), key=lambda r: abs(M[r][c]))
        M[c], M[piv] = M[piv], M[c]
        if M[c][c] == 0:
            continue
        for r in range(p):
            if r != c:
                f = M[r][c] / M[c][c]
                for k in range(c, p + 1):
                    M[r][k] -= f * M[c][k]
    beta = [M[i][p] / M[i][i] if M[i][i] != 0 else 0.0 for i in range(p)]
    return [y[i] - sum(beta[a] * X[i][a] for a in range(p)) for i in range(n)]


def cluster_spearman(xs, ys, clusters, n_perm=20000, seed=17):
    """Spearman rho with a CLUSTER-ROBUST permutation p-value.

    Why this exists, and why the plain row-level p-value below it is not
    enough: the sweep produces many rows per instrument (one per timeframe),
    and rows sharing an instrument share a price series.  Permuting rows
    assumes 68 independent observations when there are really 13.  Measured on
    the recorded run, that inflates significance by 3x: the headline
    correlation reads p = 0.031 row-wise and p = 0.103 cluster-wise, i.e. the
    difference between "significant" and "not".

    The null here is exchangeability of whole instrument blocks: reassign each
    instrument's outcomes to another instrument, keeping every block intact.
    """
    import random

    rx, ry = _ranks(xs), _ranks(ys)
    rho = _pearson(rx, ry)
    names = sorted(set(clusters))
    if len(names) < 3:
        return rho, float("nan"), len(names)
    idx = {c: [i for i, v in enumerate(clusters) if v == c] for c in names}
    rng = random.Random(seed)
    hits = 0
    for _ in range(n_perm):
        shuffled = names[:]
        rng.shuffle(shuffled)
        permuted = [0.0] * len(ry)
        for src, dst in zip(names, shuffled):
            si, di = idx[src], idx[dst]
            for k in range(len(di)):
                permuted[di[k]] = ry[si[k % len(si)]]
        if abs(_pearson(rx, permuted)) >= abs(rho):
            hits += 1
    return rho, hits / n_perm, len(names)


def partial_cluster_spearman(xs, ys, controls, clusters, n_perm=20000, seed=23):
    """Cluster-robust Spearman of xs vs ys after removing `controls` from both.

    Sample size is a SUPPRESSOR in this experiment, not a confound: it
    correlates -0.53 with the ratio (more bars resolve the order better) and
    -0.10 with excess Sharpe (bar-rich rows are the fast timeframes, which
    bleed fees), so both paths push the same way and mask the relationship.
    Controlling for it strengthens the correlation from -0.263 to -0.376.
    """
    import random

    rx, ry = _ranks(xs), _ranks(ys)
    ctrl = [_ranks(c) for c in controls]
    xr, yr = _residualize(rx, ctrl), _residualize(ry, ctrl)
    rho = _pearson(xr, yr)
    names = sorted(set(clusters))
    if len(names) < 3:
        return rho, float("nan")
    idx = {c: [i for i, v in enumerate(clusters) if v == c] for c in names}
    rng = random.Random(seed)
    hits = 0
    for _ in range(n_perm):
        shuffled = names[:]
        rng.shuffle(shuffled)
        permuted = [0.0] * len(yr)
        for src, dst in zip(names, shuffled):
            si, di = idx[src], idx[dst]
            for k in range(len(di)):
                permuted[di[k]] = yr[si[k % len(si)]]
        if abs(_pearson(xr, permuted)) >= abs(rho):
            hits += 1
    return rho, hits / n_perm


def stratified_spearman(rows, strat_key, x_key, y_key):
    """Pooled within-stratum Spearman: rank inside each stratum, centre, pool.

    This is the decomposition that carries the actual finding.  Stratifying by
    timeframe asks "does the gate rank INSTRUMENTS?" and stratifying by symbol
    asks "does it rank TIMEFRAMES?".  On the recorded run the first is -0.317
    (negative in all 7 timeframes) and the second is exactly 0.000.
    """
    px, py, per = [], [], []
    for key in sorted({r[strat_key] for r in rows}):
        sub = [r for r in rows if r[strat_key] == key]
        if len(sub) < 4:
            continue
        rx = _ranks([r[x_key] for r in sub])
        ry = _ranks([r[y_key] for r in sub])
        mx, my = sum(rx) / len(rx), sum(ry) / len(ry)
        px += [v - mx for v in rx]
        py += [v - my for v in ry]
        per.append((key, len(sub), _pearson(rx, ry)))
    return _pearson(px, py) if px else float("nan"), per


def spearman(xs, ys, n_perm=20000, seed=7):
    """Spearman rho with a two-sided ROW-LEVEL permutation p-value.

    Kept because it is what the pre-registered run reported, but see
    cluster_spearman: this p-value assumes the rows are independent, and in
    this experiment they are not.  Report both or report the cluster one.
    """
    import random

    def ranks(v):
        order = sorted(range(len(v)), key=lambda i: v[i])
        r = [0.0] * len(v)
        i = 0
        while i < len(order):
            j = i
            while j + 1 < len(order) and v[order[j + 1]] == v[order[i]]:
                j += 1
            avg = (i + j) / 2.0 + 1.0
            for k in range(i, j + 1):
                r[order[k]] = avg
            i = j + 1
        return r

    def pearson(a, b):
        n = len(a)
        ma, mb = sum(a) / n, sum(b) / n
        num = sum((x - ma) * (y - mb) for x, y in zip(a, b))
        da = math.sqrt(sum((x - ma) ** 2 for x in a))
        db = math.sqrt(sum((y - mb) ** 2 for y in b))
        return num / (da * db) if da > 0 and db > 0 else float("nan")

    rx, ry = ranks(xs), ranks(ys)
    rho = pearson(rx, ry)
    rng = random.Random(seed)
    hits = 0
    ry2 = ry[:]
    for _ in range(n_perm):
        rng.shuffle(ry2)
        if abs(pearson(rx, ry2)) >= abs(rho):
            hits += 1
    return rho, hits / n_perm


def cmd_report(args):
    min_trades = args.min_trades
    with open(args.results) as f:
        results = json.load(f)

    # Refuse to report on unusable rows rather than rank them.
    #
    # This guard exists because its absence produced a confident lie: with a
    # broken parser every ratio came back NaN, and because NaN compares False
    # against everything, sorting and ranking silently accepted them and the
    # report printed a Spearman rho and p-value computed from garbage. A
    # statistic derived from NaN must be impossible to print, not merely
    # unlikely.
    parse_errors = [r for r in results if r.get("parse_error")]
    if parse_errors:
        first = parse_errors[0]["parse_error"]
        raise SystemExit(
            "REFUSING TO REPORT: %d of %d rows failed to parse.\n  first error: %s\n"
            "Re-run 'gate_sweep.py run' against a matching cli_trader build."
            % (len(parse_errors), len(results), first))

    rows = [r for r in results if r.get("spec_ok") and r.get("bt_ok")]
    bad = [r for r in rows
           if not math.isfinite(r.get("ratio", float("nan")))
           or not math.isfinite(r.get("excess_sharpe", float("nan")))]
    if bad:
        raise SystemExit(
            "REFUSING TO REPORT: %d of %d usable rows carry a non-finite ratio or "
            "excess Sharpe (e.g. %s). Ranking them would fabricate a statistic."
            % (len(bad), len(rows), ", ".join(r["label"] for r in bad[:5])))
    if not rows:
        raise SystemExit("No rows with both a spectrum and a backtest. Nothing to report.")

    rows.sort(key=lambda r: r["ratio"])
    hdr = f"{'pair@TF':18s} {'verdict':10s} {'ratio':>6s} {'alpha':>7s} {'excess':>7s} {'+/-':>5s} {'trades':>6s} {'years':>6s} {'maxDD%':>7s}"
    print(hdr)
    print("-" * len(hdr))
    for r in rows:
        print(f"{r['label']:18s} {r['verdict']:10s} {r['ratio']:6.2f} {r.get('alpha', float('nan')):7.3f} "
              f"{r['excess_sharpe']:+7.2f} {r.get('sharpe_se', float('nan')):5.2f} {r.get('trades', 0):6d} "
              f"{r.get('years', float('nan')):6.2f} {r.get('maxdd', float('nan')):7.1f}")

    # Correlations use one row per (pair, TF): the matched-ladder rows plus the
    # 1d~full rows (which have no untagged twin). fw rows and the duplicate
    # full-depth 4h rows appear in the table but not in the statistics.
    main = lambda r: (r.get("tag") != "fw"
                      and not (r.get("tag") == "full" and r["period"] == 14400)
                      and r.get("trades", 0) >= min_trades)
    for name, sel in [
        ("ALL (>=%d trades)" % min_trades, main),
        ("CRYPTO (>=%d trades)" % min_trades, lambda r: main(r) and r["profile"] == "crypto"),
        ("EQUITY (>=%d trades)" % min_trades, lambda r: main(r) and r["profile"] == "equity"),
    ]:
        sub = [r for r in rows if sel(r)]
        if len(sub) < 5:
            print(f"\n{name}: only {len(sub)} points, skipping correlation")
            continue
        xs = [r["ratio"] for r in sub]
        ys = [r["excess_sharpe"] for r in sub]
        rho, p = spearman(xs, ys)
        _, pc, nclust = cluster_spearman(xs, ys, [r["symbol"] for r in sub])
        print(f"\n{name}: n={len(sub)}  Spearman(ratio, excess Sharpe) = {rho:+.3f}")
        print(f"  row-level perm p    = {p:.4f}   <- assumes {len(sub)} independent rows; they are not")
        print(f"  cluster-robust p    = {pc:.4f}   <- {nclust} instrument blocks; THIS is the honest one")
        print("  (gate predicts NEGATIVE rho: lower ratio = resolvable memory = better tsmom)")

    # Everything below needs sample-size and stratum fields, so it only runs on
    # rows that carry them.
    full = [r for r in rows if main(r) and r.get("candles") and r.get("years")]
    if len(full) >= 10:
        clusters = [r["symbol"] for r in full]
        prho, pp = partial_cluster_spearman(
            [r["ratio"] for r in full], [r["excess_sharpe"] for r in full],
            [[math.log(r["candles"]) for r in full], [math.log(r["years"]) for r in full]],
            clusters)
        print(f"\nControlling for sample size (log bars, log years): rho = {prho:+.3f}  "
              f"cluster-robust p = {pp:.4f}")
        print("  sample size is a SUPPRESSOR here - removing it STRENGTHENS the effect")

        print("\nDecomposition - which dimension does the gate actually rank?")
        for label, key, reading in [
            ("within timeframe (ranks INSTRUMENTS)", "period", "TF"),
            ("within instrument (ranks TIMEFRAMES)", "symbol", "symbol"),
        ]:
            pooled, per = stratified_spearman(full, key, "ratio", "excess_sharpe")
            print(f"  {label}: pooled rho = {pooled:+.3f}")
            same = sum(1 for _, _, rr in per if rr < 0)
            detail = "  ".join(
                f"{TF_NAME.get(k, k)}:{rr:+.2f}" if key == "period" else f"{k}:{rr:+.2f}"
                for k, _, rr in per)
            print(f"    {same}/{len(per)} strata negative   {detail}")

    ident = [r for r in rows if main(r) and r["verdict"] == "FRACTIONAL"]
    rest = [r for r in rows if main(r) and r["verdict"] != "FRACTIONAL"]
    if ident and rest:
        mi = sum(r["excess_sharpe"] for r in ident) / len(ident)
        mr = sum(r["excess_sharpe"] for r in rest) / len(rest)
        print(f"\nGate summary (>= {min_trades} trades): identified n={len(ident)} mean excess {mi:+.2f} "
              f"| not identified n={len(rest)} mean excess {mr:+.2f}")


def add_layout_args(p):
    p.add_argument("--repo", metavar="DIR",
                   help=f"repo root (default: two levels above this script, {REPO_ROOT})")
    p.add_argument("--data-dir", metavar="DIR",
                   help="pulled venue stores (default: <repo>/data)")
    p.add_argument("--derived-dir", metavar="DIR",
                   help="resampled stores (default: <data-dir>/derived)")
    p.add_argument("--fw-dir", metavar="DIR",
                   help="matched-window truncations (default: <derived-dir>/fw)")
    p.add_argument("--cli", metavar="BIN",
                   help="cli_trader binary (default: <repo>/build/cli_trader)")


def layout_from_args(args):
    return Layout(repo=args.repo, data=args.data_dir, derived=args.derived_dir,
                  fw=args.fw_dir, cli=args.cli)


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="gate_sweep.py",
        description="Order-gate sweep: rank-correlate the spiral estimator's "
                    "identification ratio against tsmom excess Sharpe over 98 "
                    "(instrument, timeframe) candle stores.",
        epilog="Prediction: NEGATIVE Spearman (resolvable memory order = tsmom "
               "beats buy-and-hold). Recorded run: rho -0.263, perm p 0.031. "
               "See the module docstring for the full store manifest.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True, metavar="{run,report,manifest}")

    p_run = sub.add_parser("run", help="run order-spectrum + tsmom backtest per store (~30 min), write results JSON")
    add_layout_args(p_run)
    p_run.add_argument("--results", default=str(DEFAULT_RESULTS), metavar="JSON",
                       help=f"output file (default: {DEFAULT_RESULTS}) - OVERWRITES the recorded run")
    p_run.set_defaults(func=cmd_run)

    p_rep = sub.add_parser("report", help="table + Spearman correlations from an existing results JSON")
    p_rep.add_argument("--results", default=str(DEFAULT_RESULTS), metavar="JSON",
                       help=f"input file (default: {DEFAULT_RESULTS})")
    p_rep.add_argument("--min-trades", type=int, default=8, metavar="N",
                       help="drop rows with fewer trades from the correlations "
                            "(default: 8, the pre-registered threshold)")
    p_rep.set_defaults(func=cmd_report)

    p_man = sub.add_parser("manifest", help="print the store manifest and which files are present")
    add_layout_args(p_man)
    p_man.set_defaults(func=cmd_manifest)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    main()
