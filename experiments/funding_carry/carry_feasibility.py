#!/usr/bin/env python3
"""Delta-neutral carry on Poloniex perps: feasibility on 180 days of REAL
funding settlements (data/funding/*.csv, preserved by scripts/record_funding.py).

The trade: long 1.0 notional spot, short 1.0 notional perp. Price cancels;
P&L = funding received - fees - basis drift between the legs.

PRE-REGISTERED before any run (this file is the registration):
  R1 always-on: enter day one, hold to the end of data.
  R2 negative-filter: exit both legs after a NEGATIVE settlement, re-enter
     after the next positive one. One rule, zero tuned numbers.
Costs, stated first: spot 0.125%/side + perp 0.075%/side + 0.05% slippage
per leg per side = 0.60% per full round trip. Capital = 1.5x notional
(spot fully funded + perp margin at 2x), so yield-on-capital = P&L / 1.5.
Basis: measured from daily perp closes vs spot 4h-store closes at the same
UTC day; entry/exit pay any adverse basis, marks show the wobble.
"""
import csv, json, os, sys, time, urllib.request, struct, datetime as dt, statistics

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PAIRS = ["BTC","ETH","XRP","LTC","DOGE","TRX","SOL"]   # ADA excluded: 45 days only
FEE_RT = 2*(0.00125+0.0005) + 2*(0.00075+0.0005)       # 0.0060 full round trip
CAPITAL_MULT = 1.5

def funding(sym):
    rows=[]
    with open(os.path.join(REPO,"data","funding",f"{sym}_USDT_PERP.csv")) as f:
        for r in csv.DictReader(f):
            rows.append((int(r["settle_ms"]), float(r["rate"])))
    return sorted(rows)

def perp_daily(sym):
    out={}; now=int(time.time()*1000); eT=now
    for _ in range(4):
        u=(f"https://api.poloniex.com/v3/market/candles?symbol={sym}_USDT_PERP"
           f"&interval=DAY_1&sT={now-185*86400000}&eT={eT}&limit=100")
        with urllib.request.urlopen(u,timeout=20) as r: d=json.load(r).get("data") or []
        if not d: break
        for row in d:
            day=int(int(row[7])//86400000)
            out[day]=float(row[3])                      # close
        eT=min(int(r[7]) for r in d)-1
        time.sleep(0.15)
    return out

def spot_daily(sym):
    path=os.path.join(REPO,"data",f"{sym}_USDT_14400.ctc")
    raw=open(path,"rb").read(); slen=struct.unpack_from("<I",raw,12)[0]; off=16+slen
    out={}
    for i in range((len(raw)-off)//48):
        t,o,h,l,c,v=struct.unpack_from("<qddddd",raw,off+i*48)
        out[t//86400]=c                                  # last 4h close of the day wins
    return out

def simulate(sym, rule):
    F=funding(sym)
    in_pos = rule=="R1"
    pnl=0.0; entries=1 if in_pos else 0; neg_hits=0
    for i,(t,r) in enumerate(F):
        if rule=="R2":
            if in_pos and r<0:
                pnl+=r; neg_hits+=1; in_pos=False; pnl-=FEE_RT/2   # exit after the hit
                continue
            if not in_pos and r>0:
                in_pos=True; entries+=1; pnl-=FEE_RT/2
                continue
        if in_pos:
            pnl+=r
            if r<0: neg_hits+=1
    if rule=="R1": pnl-=FEE_RT                            # one entry + one exit
    elif in_pos: pnl-=FEE_RT/2                            # close the last leg
    days=(F[-1][0]-F[0][0])/86400000
    return pnl, entries, neg_hits, days

print(f"{'pair':<6}{'rule':<4}{'net on notional':>16}{'ann.':>8}{'on capital':>12}{'entries':>9}{'neg hits':>9}")
print("-"*66)
port={}
for sym in PAIRS:
    for rule in ("R1","R2"):
        pnl,entries,neg,days=simulate(sym,rule)
        ann=pnl*365/days*100
        port.setdefault(rule,[]).append(ann)
        print(f"{sym:<6}{rule:<4}{pnl*100:>15.2f}%{ann:>7.1f}%{ann/CAPITAL_MULT:>11.1f}%{entries:>9}{neg:>9}")
print("-"*66)
for rule in ("R1","R2"):
    m=statistics.mean(port[rule])
    print(f"equal-weight 7-pair {rule}: {m:.1f}%/yr on notional = {m/CAPITAL_MULT:.1f}%/yr on capital")

print("\nbasis wobble (perp daily close vs spot daily close), abs %:")
for sym in PAIRS:
    try:
        p=perp_daily(sym); s=spot_daily(sym)
        common=sorted(set(p)&set(s))[-170:]
        if len(common)<30: print(f"  {sym:<6} insufficient overlap"); continue
        b=[abs(p[d]/s[d]-1)*100 for d in common]
        print(f"  {sym:<6} n={len(b):3d}  median {statistics.median(b):.3f}%  p95 {sorted(b)[int(.95*len(b))]:.3f}%  max {max(b):.3f}%")
    except Exception as e:
        print(f"  {sym:<6} {type(e).__name__}: {e}")
