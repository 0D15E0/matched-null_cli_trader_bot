#!/usr/bin/env python3
"""Engine-level evaluation of `factor_trend` on DEVELOPMENT data only.

Runs the portfolio command for a fixed, small set of configurations and writes
every parsed result plus the exact command to results.json. Nothing here can
touch data after 2023-12-31: every window is asserted against the frozen
holdout boundary before the binary is invoked.

Sections (all pre-2024):
  dose_response  4-env research universe, 2018-01-01..2023-12-31, vt 0.20,
                 factorWeight in {0, .25, .5, .75, 1} plus the two incumbents
  folds          the research protocol's three folds: registered candidate
                 (w=0.5), its w=0 anchor, tsmom at live defaults, ensemble 2/0
  confirmation   the EXPLORATORY entry-confirmation knob (second dev look)
  eight_coin     the deployment universe, 2021-01-01..2023-12-31, live sizing
                 (vt 0.30, vol-window 90) and the real fee, descriptive
"""
import json, re, subprocess, sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
BINARY = ROOT / "build" / "cli_trader"
DEV_END = "2023-12-31"
ENVS4 = "BTC_USDT:14400,ETH_USDT:14400,XRP_USDT:14400,LTC_USDT:14400"
ENVS8 = ENVS4 + ",DOGE_USDT:14400,TRX_USDT:14400,ADA_USDT:14400,SOL_USDT:14400"
FOLDS = (("2018-01-01", "2019-12-31"), ("2020-01-01", "2021-12-31"), ("2022-01-01", "2023-12-31"))
DEV = ("2018-01-01", DEV_END)
DEPLOY = ("2021-01-01", DEV_END)

def portfolio(envs, start, end, strategy, sparams="", vt=0.20, vol_window=None, fee=None, slip=None):
    assert end <= DEV_END, f"refusing to evaluate past the frozen boundary: {end}"
    cmd = [str(BINARY), "portfolio", "--envs", envs, "--start", start, "--end", end,
           "--warmup-bars", "80", "--strategy", strategy, "--vol-target", str(vt),
           "--weights", "equal", "--vol-lookback", "120", "--rebalance", "30"]
    if sparams: cmd += ["--sparams", sparams]
    if vol_window is not None: cmd += ["--vol-window", str(vol_window)]
    if fee is not None: cmd += ["--fee", str(fee)]
    if slip is not None: cmd += ["--slippage", str(slip)]
    out = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
    if out.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)}\n{out.stderr}")
    t = out.stdout
    m = re.search(r"^Sharpe \(ann\.\):\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)", t, re.M)
    se = re.search(r"^\s*\+/-\s+([-\d.]+)", t, re.M)
    dd = re.search(r"^Max drawdown:\s+([-\d.]+)%\s+([-\d.]+)%", t, re.M)
    corr = re.search(r"Avg pairwise sleeve correlation:\s+([-\d.]+)", t)
    mean = re.search(r"Mean single-instrument Sharpe:\s+([-\d.]+)", t)
    cagr = re.search(r"^CAGR:\s+([-\d.]+)%\s+([-\d.]+)%", t, re.M)
    sleeves = []
    for row in re.finditer(r"^\s+([A-Z]+_USDT)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+(\d+)", t, re.M):
        sleeves.append({"symbol": row.group(1), "sharpe": float(row.group(2)), "bh_sharpe": float(row.group(3)),
                        "excess": float(row.group(4)), "max_dd_pct": float(row.group(5)), "trades": int(row.group(6))})
    return {"command": cmd, "sharpe": float(m.group(1)), "basket_sharpe": float(m.group(2)),
            "excess_sharpe": float(m.group(3)), "sharpe_se": float(se.group(1)),
            "max_dd_pct": float(dd.group(1)), "basket_max_dd_pct": float(dd.group(2)),
            "cagr_pct": float(cagr.group(1)), "basket_cagr_pct": float(cagr.group(2)),
            "sleeve_corr": float(corr.group(1)), "mean_sleeve_sharpe": float(mean.group(1)) if mean else None,
            "trades": sum(s["trades"] for s in sleeves), "sleeves": sleeves}

def row(label, r):
    return (f"| {label} | {r['sharpe']:.2f} | {r['excess_sharpe']:+.2f} | {r['max_dd_pct']:.1f}% | "
            f"{r['sleeve_corr']:.3f} | {r['mean_sleeve_sharpe'] if r['mean_sleeve_sharpe'] is not None else float('nan'):.2f} | {r['trades']} |")

HEAD = "| configuration | Sharpe | excess vs basket | max DD | sleeve corr | mean sleeve Sharpe | trades |\n|---|---:|---:|---:|---:|---:|---:|"

results = {"generated_at": datetime.now(timezone.utc).isoformat(), "development_end": DEV_END,
           "holdout_accessed": False, "sections": {}}

print("## Dose-response, 4-env research universe, %s..%s, vt 0.20\n" % DEV); print(HEAD)
sec = []
for w in (0.0, 0.25, 0.5, 0.75, 1.0):
    r = portfolio(ENVS4, *DEV, "factor_trend", f"factorWeight={w}"); r["label"] = f"factor_trend w={w:g}"; sec.append(r); print(row(r["label"], r))
for label, strat, sp in (("tsmom, live defaults (5% abs, vol gate)", "tsmom", ""), ("ensemble_vote 2/0 (live rule)", "ensemble_vote", "enterVotes=2,exitVotes=0")):
    r = portfolio(ENVS4, *DEV, strat, sp); r["label"] = label; sec.append(r); print(row(label, r))
results["sections"]["dose_response"] = sec

print("\n## Protocol folds, 4-env, vt 0.20 (Sharpe / excess vs basket / max DD)\n")
print("| configuration | 2018-19 | 2020-21 | 2022-23 | mean excess | worst excess |\n|---|---|---|---|---:|---:|")
sec = []
for label, sp, strat in (("factor_trend w=0.5 (registered)", "factorWeight=0.5", "factor_trend"),
                         ("anchor w=0 (tsmom, 0.5 sigma, no gate)", "factorWeight=0", "factor_trend"),
                         ("tsmom, live defaults", "", "tsmom"),
                         ("ensemble_vote 2/0", "enterVotes=2,exitVotes=0", "ensemble_vote")):
    cells = []; folds = []
    for start, end in FOLDS:
        r = portfolio(ENVS4, start, end, strat, sp); r["fold"] = [start, end]; folds.append(r)
        cells.append(f"{r['sharpe']:.2f} / {r['excess_sharpe']:+.2f} / {r['max_dd_pct']:.1f}%")
    ex = [f["excess_sharpe"] for f in folds]
    print(f"| {label} | " + " | ".join(cells) + f" | {sum(ex)/3:+.2f} | {min(ex):+.2f} |")
    sec.append({"label": label, "folds": folds, "mean_excess": sum(ex) / 3, "worst_excess": min(ex)})
results["sections"]["folds"] = sec

print("\n## Exploratory entry confirmation (SECOND dev look), own-only signal w=0, Bitcoin z must exceed g to enter\n")
sec = []
for envs, window, vt, vw, fee, slip, name in ((ENVS4, DEV, 0.20, None, None, None, "4-env research universe, vt 0.20"),
                                             (ENVS8, DEPLOY, 0.30, 90, 0.00125, 0.0005, "8-coin deployment universe 2021-23, vt 0.30/90, real fee")):
    print(f"\n### {name}\n"); print(HEAD)
    for g in (-9.0, -0.5, 0.0, 0.5):
        r = portfolio(envs, *window, "factor_trend", f"factorWeight=0,confirmZ={g}", vt, vw, fee, slip)
        r["label"] = "confirmation off (anchor)" if g < -8 else f"confirm g={g:g}"; r["universe"] = name; sec.append(r); print(row(r["label"], r))
results["sections"]["confirmation"] = sec

print("\n## Deployment universe (8 coins), %s..%s, vt 0.30 / vol-window 90, fee 0.125%% + slip 0.05%% (descriptive)\n" % DEPLOY); print(HEAD)
sec = []
for label, strat, sp in (("LIVE ensemble_vote 2/0", "ensemble_vote", "enterVotes=2,exitVotes=0"),
                         ("anchor w=0 (tsmom, 0.5 sigma, no gate)", "factor_trend", "factorWeight=0"),
                         ("factor_trend w=0.25", "factor_trend", "factorWeight=0.25"),
                         ("factor_trend w=0.5", "factor_trend", "factorWeight=0.5"),
                         ("factor_trend w=0.75", "factor_trend", "factorWeight=0.75"),
                         ("factor_trend w=1", "factor_trend", "factorWeight=1")):
    r = portfolio(ENVS8, *DEPLOY, strat, sp, 0.30, 90, 0.00125, 0.0005); r["label"] = label; sec.append(r); print(row(label, r))
results["sections"]["eight_coin"] = sec

out = HERE / "results.json"
out.write_text(json.dumps(results, indent=2) + "\n")
print(f"\nwrote {out}")
