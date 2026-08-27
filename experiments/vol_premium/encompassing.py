#!/usr/bin/env python3
"""THE decisive test for Path B: does our volatility forecast carry information
that the market's implied volatility does not?

THE QUESTION
------------
This repo's fractional volatility forecast beats a TRAILING-window estimator by
57.8% on QLIKE at a ~24-day horizon.  That is not the relevant benchmark.  A
trade only exists if the forecast beats what the option market already charges,
because the market's implied volatility IS a forecast - produced by people who
also know about volatility clustering.

So run the encompassing (Mincer-Zarnowitz) regression

    realized[t, t+T]  =  a  +  b * implied[t]  +  c * forecast[t]  +  eps

    c ~ 0  =>  implied already contains everything we know.  NO EDGE.  STOP.
    c != 0 =>  our forecast adds information the price does not have, and the
               sign and size of c define the trade.

WHY THE STANDARD ERRORS MATTER MORE THAN THE COEFFICIENT
--------------------------------------------------------
The dependent variable is a 30-day FORWARD window sampled daily, so consecutive
observations share ~29/30 of their data.  Residuals are therefore massively
autocorrelated and ordinary OLS standard errors are meaningless - they will
hand you a t-statistic several times too large.  This is the same failure mode
as the row-level permutation p-value that had to be retracted from the
order-gate experiment (p = 0.031 became 0.103 once dependence was respected).
Every standard error here is Newey-West HAC, and the lag length is reported and
varied.

DATA
----
implied   Deribit DVOL, model-free 30-day BTC implied vol (fetch_dvol.py)
forecast  the SHIPPED forecaster, indicators/vol_forecast.h, VolModel
          FractionalHAR at horizonBars = 180 (= 30 days of 4h bars), causal by
          construction (out[i] uses only close[0..i]); dumped by the
          vol_forecast_dump tool
realized  annualized stddev of 4h log returns over the NEXT 180 bars, zero-mean
          convention (the second moment is what a variance contract pays)

All three are annualized decimals (0.65 = 65%).
"""
import argparse
import csv
import sys
from datetime import datetime, timezone
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent


def load_dvol(path):
    out = {}
    with open(path) as f:
        for row in csv.DictReader(f):
            out[row["date"]] = float(row["dvol_close"]) / 100.0
    return out


def load_dump(path):
    """date -> (forecast, realized), keeping the LAST bar of each UTC day so the
    reading is 'what we knew at the end of that day', matching DVOL's close."""
    per_day = {}
    with open(path) as f:
        for row in csv.DictReader(f):
            if not row["forecast_annual"] or not row["realized_annual"]:
                continue
            ts = int(row["timestamp"])
            d = datetime.fromtimestamp(ts, timezone.utc).strftime("%Y-%m-%d")
            prev = per_day.get(d)
            if prev is None or ts > prev[0]:
                per_day[d] = (ts, float(row["forecast_annual"]),
                              float(row["realized_annual"]))
    return {d: (v[1], v[2]) for d, v in per_day.items()}


def newey_west(X, e, lags):
    """Bartlett-kernel HAC meat matrix: S = sum_t e_t^2 x_t x_t' +
    sum_l w_l sum_t e_t e_{t-l} (x_t x_{t-l}' + x_{t-l} x_t')."""
    n, k = X.shape
    S = (X * e[:, None]).T @ (X * e[:, None])
    for l in range(1, lags + 1):
        w = 1.0 - l / (lags + 1.0)
        A = (X[l:] * e[l:, None]).T @ (X[:-l] * e[:-l, None])
        S += w * (A + A.T)
    return S


def ols_hac(y, X, lags):
    n, k = X.shape
    XtX_inv = np.linalg.pinv(X.T @ X)
    beta = XtX_inv @ (X.T @ y)
    e = y - X @ beta
    S = newey_west(X, e, lags)
    V = XtX_inv @ S @ XtX_inv
    se = np.sqrt(np.maximum(np.diag(V), 0.0))
    tss = float(((y - y.mean()) ** 2).sum())
    r2 = 1.0 - float((e ** 2).sum()) / tss if tss > 0 else float("nan")
    return beta, se, r2, e


def report(name, y, X, names, lags):
    beta, se, r2, _ = ols_hac(y, X, lags)
    print(f"  {name}   (n={len(y)}, HAC lags={lags}, R2={r2:.3f})")
    for nm, b, s in zip(names, beta, se):
        t = b / s if s > 0 else float("nan")
        star = ""
        if abs(t) > 2.576: star = "  ***"
        elif abs(t) > 1.960: star = "  **"
        elif abs(t) > 1.645: star = "  *"
        print(f"    {nm:<12s} {b:+9.4f}   HAC se {s:7.4f}   t = {t:+7.2f}{star}")
    return beta, se, r2


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dvol", default=str(HERE / "dvol_btc.csv"))
    ap.add_argument("--dump", required=True,
                    help="CSV from vol_forecast_dump (horizon must match DVOL's 30d)")
    ap.add_argument("--lags", type=int, default=40,
                    help="Newey-West lag length; should exceed the 30-day overlap")
    args = ap.parse_args()

    dvol = load_dvol(args.dvol)
    dump = load_dump(args.dump)
    days = sorted(set(dvol) & set(dump))
    if len(days) < 200:
        print(f"only {len(days)} overlapping days; need more history", file=sys.stderr)
        return 1

    imp = np.array([dvol[d] for d in days])
    fc = np.array([dump[d][0] for d in days])
    rv = np.array([dump[d][1] for d in days])
    ones = np.ones(len(days))

    print("=" * 74)
    print("ENCOMPASSING REGRESSION: does our forecast beat DVOL?")
    print("=" * 74)
    print(f"sample      {days[0]} .. {days[-1]}   ({len(days)} daily observations)")
    print(f"means       implied {imp.mean():.4f}   forecast {fc.mean():.4f}   "
          f"realized {rv.mean():.4f}")
    print(f"variance risk premium (implied - realized) = {imp.mean()-rv.mean():+.4f}"
          f"  ({100*(imp.mean()-rv.mean()):+.2f} vol points)")
    print(f"correlations   implied~realized {np.corrcoef(imp,rv)[0,1]:+.3f}   "
          f"forecast~realized {np.corrcoef(fc,rv)[0,1]:+.3f}   "
          f"implied~forecast {np.corrcoef(imp,fc)[0,1]:+.3f}")

    print("\n--- 1. baselines, each forecaster alone -------------------------------")
    report("implied only ", rv, np.column_stack([ones, imp]), ["const", "implied"], args.lags)
    report("forecast only", rv, np.column_stack([ones, fc]), ["const", "forecast"], args.lags)

    print("\n--- 2. THE TEST: both together ---------------------------------------")
    beta, se, r2 = report("encompassing ", rv, np.column_stack([ones, imp, fc]),
                          ["const", "implied", "forecast"], args.lags)
    c, sc = beta[2], se[2]
    tc = c / sc if sc > 0 else float("nan")

    print("\n--- 3. same in LOGS (closer to homoskedastic) ------------------------")
    lr, li, lf = np.log(rv), np.log(imp), np.log(fc)
    report("log encompass", lr, np.column_stack([ones, li, lf]),
           ["const", "log implied", "log forecast"], args.lags)

    print("\n--- 4. HAC lag sensitivity on c --------------------------------------")
    for L in (20, 30, 40, 60, 90):
        b2, s2, _, _ = ols_hac(rv, np.column_stack([ones, imp, fc]), L)
        print(f"    lags={L:3d}   c = {b2[2]:+.4f}   se {s2[2]:.4f}   "
              f"t = {b2[2]/s2[2]:+.2f}")

    print("\n--- 5. split-sample stability of c ----------------------------------")
    half = len(days) // 2
    for label, sl in (("first half", slice(0, half)), ("second half", slice(half, None))):
        b2, s2, _, _ = ols_hac(rv[sl], np.column_stack([ones[sl], imp[sl], fc[sl]]), args.lags)
        print(f"    {label:<12s} ({days[sl][0] if isinstance(days[sl], list) else days[sl.start or 0]} ..) "
              f"c = {b2[2]:+.4f}   t = {b2[2]/s2[2] if s2[2]>0 else float('nan'):+.2f}")

    print("\n" + "=" * 74)
    print("VERDICT")
    print("=" * 74)
    if abs(tc) < 1.96:
        print(f"c = {c:+.4f} with HAC t = {tc:+.2f} -- NOT significant at 5%.")
        print("Implied volatility already contains what our forecast knows.")
        print("NO EDGE on this test. Path B does not open. Do not build the")
        print("options stack on this signal.")
    else:
        print(f"c = {c:+.4f} with HAC t = {tc:+.2f} -- significant.")
        print("Our forecast carries information beyond implied. Before believing")
        print("it: confirm the sign is stable across BOTH halves above, that the")
        print("lag sensitivity does not straddle significance, and that b on")
        print("implied is not absurd (a spurious c often comes with a broken b).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
