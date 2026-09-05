#!/usr/bin/env python3
"""Does Bitcoin's trend carry information about an altcoin's next bar that the
altcoin's OWN trend does not?  (Measurement BEFORE any strategy is built.)

Motivation: every strategy family in this repo reads one price series. Crypto
is close to a one-factor market (alt = beta * market + idiosyncratic noise, with
beta > 1 and large noise), and the equity literature finds that momentum in
individual assets is largely FACTOR momentum (Ehsani & Linnainmaa, J. Finance
2022). If that holds here, the market factor's trend - measured on BTC, the
least noisy proxy - should predict an alt's return beyond the alt's own trend.

PRE-REGISTERED before the first run (this docstring is the registration):
  * Signal:  z = log(close[t]/close[t-90]) / (sigma_30 * sqrt(90)), the tsmom
             statistic at the live book's defaults, on the coin (own_z) and on
             BTC_USDT (mkt_z) - BTC read at the latest bar <= t - 4h (one bar
             of lag, so a live alt sleeve never needs a BTC bar that may not
             have been fetched yet).
  * Target:  the TRADABLE next-bar return, open[t+1] -> open[t+2], i.e. a
             decision at close[t] filled at the next open and marked one bar
             later. Pooled over the 7 alts (BTC excluded: for it mkt == own).
  * Window:  development data only, timestamps < 2024-01-01.
  * Test 1:  2x2 table by sign(own_z) x sign(mkt_z). The factor carries
             information iff (own>0, mkt<0) is materially worse than
             (own>0, mkt>0) AND (own<0, mkt>0) is materially better than
             (own<0, mkt<0). Report per-coin agreement counts, not just pools.
  * Test 2:  dose-response of blend = (1-w)*own_z + w*mkt_z for w in
             {0, .25, .5, .75, 1}: Sharpe of "hold while blend > 0.5" bars.
             A monotone curve is a mechanism; a spike at one w is noise.
  * Test 3:  the same with a leave-one-out equal-weight basket z instead of
             BTC, to see whether averaging the factor beats reading it off BTC.
Statistics are signal-level (no fees, no sizing) - this decides whether a
strategy is worth BUILDING, not whether it is worth trading; the engine and the
research protocol decide the latter.
"""
import math, os, struct, sys
from collections import defaultdict

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(REPO, "data")
PERIOD = 14400
DEV_END = 1704067200          # 2024-01-01T00:00Z, frozen holdout boundary
DEV_START = int(os.environ.get("FT_START", "0"))   # optional: skip thin early history
COINS = ["BTC", "ETH", "XRP", "LTC", "DOGE", "TRX", "ADA", "SOL"]
L, W = 90, 30                 # tsmom defaults: lookback, vol window
LAG_BARS = 1
BARS_PER_YEAR = 365.25 * 24 / 4

def load(sym):
    path = os.path.join(DATA, f"{sym}_USDT_{PERIOD}.ctc")
    raw = open(path, "rb").read()
    assert raw[:4] == b"CTC1", path
    slen = struct.unpack_from("<I", raw, 12)[0]
    off = 16 + slen
    ts, op, cl = [], [], []
    for i in range((len(raw) - off) // 48):
        t, o, h, l, c, v = struct.unpack_from("<qddddd", raw, off + i * 48)
        if t >= DEV_END: break
        if t < DEV_START: continue
        ts.append(t); op.append(o); cl.append(c)
    return ts, op, cl

def zscore(cl):
    n = len(cl)
    lr = [math.nan] + [math.log(cl[i] / cl[i - 1]) for i in range(1, n)]
    z = [math.nan] * n
    s = ss = 0.0; cnt = 0
    for i in range(1, n):
        s += lr[i]; ss += lr[i] * lr[i]; cnt += 1
        if cnt > W:
            s -= lr[i - W]; ss -= lr[i - W] ** 2; cnt -= 1
        if cnt == W and i >= L:
            m = s / W; var = ss / W - m * m
            sd = math.sqrt(var) if var > 0 else 0.0
            if sd > 0:
                z[i] = math.log(cl[i] / cl[i - L]) / (sd * math.sqrt(L))
    return z

def align_lagged(ref_ts, ref_z, ts):
    """ref value at the latest ref bar with timestamp <= t - LAG_BARS*PERIOD."""
    out = [math.nan] * len(ts); j = 0
    for i, t in enumerate(ts):
        target = t - LAG_BARS * PERIOD
        while j + 1 < len(ref_ts) and ref_ts[j + 1] <= target: j += 1
        if ref_ts[j] <= target: out[i] = ref_z[j]
    return out

def stats(rows):
    n = len(rows)
    if n < 2: return n, math.nan, math.nan, math.nan
    m = sum(rows) / n
    v = sum((r - m) ** 2 for r in rows) / (n - 1)
    sd = math.sqrt(v)
    ann = m * BARS_PER_YEAR
    sh = (m / sd) * math.sqrt(BARS_PER_YEAR) if sd > 0 else math.nan
    return n, ann * 100, sd * math.sqrt(BARS_PER_YEAR) * 100, sh

series = {c: load(c) for c in COINS}
z_own = {c: zscore(series[c][2]) for c in COINS}
btc_ts, _, _ = series["BTC"]
z_mkt_btc = {c: align_lagged(btc_ts, z_own["BTC"], series[c][0]) for c in COINS}

# Leave-one-out basket z: mean of the OTHER coins' lagged z at this timestamp.
lagged_all = {c: {} for c in COINS}
for c in COINS:
    ts_c = series[c][0]
    for c2 in COINS:
        if c2 == c: continue
        al = align_lagged(series[c2][0], z_own[c2], ts_c)
        for i, v in enumerate(al):
            if not math.isnan(v): lagged_all[c].setdefault(i, []).append(v)
z_mkt_basket = {c: [ (sum(lagged_all[c][i]) / len(lagged_all[c][i])) if i in lagged_all[c] and len(lagged_all[c][i]) >= 2 else math.nan
                     for i in range(len(series[c][0])) ] for c in COINS}

def fwd(op, i):
    return math.log(op[i + 2] / op[i + 1]) if i + 2 < len(op) and op[i + 1] > 0 else math.nan

def table(label, zm):
    print(f"\n=== {label}: pooled over 7 alts, dev data (<2024), tradable next-bar return ===")
    print(f"{'state':<22}{'bars':>8}{'share':>8}{'ann.ret%':>10}{'ann.vol%':>10}{'Sharpe':>8}   per-coin Sharpe (ETH XRP LTC DOGE TRX ADA SOL)")
    states = [("own>0, mkt>0", 1, 1), ("own>0, mkt<0", 1, -1), ("own<0, mkt>0", -1, 1), ("own<0, mkt<0", -1, -1)]
    pooled = {s[0]: [] for s in states}; percoin = {s[0]: {} for s in states}
    total = 0
    for c in COINS:
        if c == "BTC": continue
        ts, op, cl = series[c]
        for i in range(len(ts)):
            a, b = z_own[c][i], zm[c][i]
            if math.isnan(a) or math.isnan(b): continue
            r = fwd(op, i)
            if math.isnan(r): continue
            total += 1
            for name, sa, sb in states:
                if (a > 0) == (sa > 0) and (b > 0) == (sb > 0):
                    pooled[name].append(r); percoin[name].setdefault(c, []).append(r)
    for name, _, _ in states:
        n, ret, vol, sh = stats(pooled[name])
        pc = " ".join(f"{stats(percoin[name].get(c, []))[3]:5.2f}" for c in COINS if c != "BTC")
        print(f"{name:<22}{n:>8}{n/total:>8.1%}{ret:>10.1f}{vol:>10.1f}{sh:>8.2f}   {pc}")
    # agreement counts
    agree_up = sum(1 for c in COINS if c != "BTC" and stats(percoin["own>0, mkt>0"].get(c, []))[3] > stats(percoin["own>0, mkt<0"].get(c, []))[3])
    agree_dn = sum(1 for c in COINS if c != "BTC" and stats(percoin["own<0, mkt>0"].get(c, []))[3] > stats(percoin["own<0, mkt<0"].get(c, []))[3])
    print(f"coins where mkt>0 beats mkt<0 given own>0: {agree_up}/7;  given own<0: {agree_dn}/7")
    # single-signal rows for reference
    for lab, pick in (("own>0 (any mkt)", lambda a, b: a > 0), ("mkt>0 (any own)", lambda a, b: b > 0),
                      ("own<0 (any mkt)", lambda a, b: a < 0), ("mkt<0 (any own)", lambda a, b: b < 0)):
        rows = []
        for c in COINS:
            if c == "BTC": continue
            ts, op, cl = series[c]
            for i in range(len(ts)):
                a, b = z_own[c][i], zm[c][i]
                if math.isnan(a) or math.isnan(b): continue
                r = fwd(op, i)
                if not math.isnan(r) and pick(a, b): rows.append(r)
        n, ret, vol, sh = stats(rows)
        print(f"{lab:<22}{n:>8}{n/total:>8.1%}{ret:>10.1f}{vol:>10.1f}{sh:>8.2f}")

def dose(label, zm, theta=0.5):
    print(f"\n--- {label}: dose-response, hold while blend=(1-w)*own+w*mkt > {theta} (pre-fee) ---")
    print(f"{'w':>5}{'bars long':>10}{'share':>8}{'ann.ret%':>10}{'Sharpe':>8}   per-coin Sharpe")
    for w in (0.0, 0.25, 0.5, 0.75, 1.0):
        rows = []; pc = {}
        for c in COINS:
            if c == "BTC": continue
            ts, op, cl = series[c]; tot = 0
            for i in range(len(ts)):
                a, b = z_own[c][i], zm[c][i]
                if math.isnan(a) or math.isnan(b): continue
                r = fwd(op, i)
                if math.isnan(r): continue
                tot += 1
                if (1 - w) * a + w * b > theta:
                    rows.append(r); pc.setdefault(c, []).append(r)
        n, ret, vol, sh = stats(rows)
        pcs = " ".join(f"{stats(pc.get(c, []))[3]:5.2f}" for c in COINS if c != "BTC")
        print(f"{w:>5.2f}{n:>10}{'':>8}{ret:>10.1f}{sh:>8.2f}   {pcs}")

def gate(label, zm, theta=0.5):
    print(f"\n--- {label}: CONFIRMATION gate, hold while own > {theta} AND mkt > g (pre-fee) ---")
    print(f"{'g':>6}{'bars long':>10}{'ann.ret%':>10}{'ann.vol%':>10}{'Sharpe':>8}   per-coin Sharpe")
    for g in (-99.0, -1.0, -0.5, 0.0, 0.5, 1.0):
        rows = []; pc = {}
        for c in COINS:
            if c == "BTC": continue
            ts, op, cl = series[c]
            for i in range(len(ts)):
                a, b = z_own[c][i], zm[c][i]
                if math.isnan(a) or math.isnan(b): continue
                r = fwd(op, i)
                if math.isnan(r): continue
                if a > theta and b > g:
                    rows.append(r); pc.setdefault(c, []).append(r)
        n, ret, vol, sh = stats(rows)
        pcs = " ".join(f"{stats(pc.get(c, []))[3]:5.2f}" for c in COINS if c != "BTC")
        lab = "off" if g < -50 else f"{g:.2f}"
        print(f"{lab:>6}{n:>10}{ret:>10.1f}{vol:>10.1f}{sh:>8.2f}   {pcs}")

table("TEST 1, factor = BTC (lag 1 bar)", z_mkt_btc)
gate("TEST 1b, factor = BTC", z_mkt_btc)
dose("TEST 2, factor = BTC", z_mkt_btc)
table("TEST 3, factor = leave-one-out basket mean z (lag 1 bar)", z_mkt_basket)
gate("TEST 3c, factor = basket", z_mkt_basket)
dose("TEST 3b, factor = basket", z_mkt_basket)

# Sanity: how correlated are own_z and mkt_z? (if ~1, there is nothing to learn)
print("\ncorr(own_z, mkt_z_btc) per alt:")
for c in COINS:
    if c == "BTC": continue
    xs = [(z_own[c][i], z_mkt_btc[c][i]) for i in range(len(series[c][0])) if not math.isnan(z_own[c][i]) and not math.isnan(z_mkt_btc[c][i])]
    n = len(xs); mx = sum(x for x, _ in xs) / n; my = sum(y for _, y in xs) / n
    sxy = sum((x - mx) * (y - my) for x, y in xs); sxx = sum((x - mx) ** 2 for x, _ in xs); syy = sum((y - my) ** 2 for _, y in xs)
    print(f"  {c:<5} n={n:>6}  corr={sxy / math.sqrt(sxx * syy):.3f}")
