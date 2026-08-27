#!/usr/bin/env python3
"""Preserve Poloniex perpetual funding-rate history before it evaporates.

The venue keeps only ~180 days of funding history, and its docs are wrong
about how to ask for it (discovered 2026-08-26 via a StackOverflow-shaped
bug hunt): sT/eT must be MILLISECONDS (docs say seconds; seconds land in
1970 and return errors/nothing), and limit must be <= 100 (docs say 1000;
larger values return 400 "Param error limit", which the first probe
swallowed as "no history"). This script exists because of that afternoon.

Runs daily from a systemd timer. For each perp: fetch the last 7 days of
settlements (3/day; 7 days of overlap makes any single failed run harmless),
merge into data/funding/<SYM>.csv keyed on settlement timestamp, rewrite
atomically. Read-only public endpoint, no keys, ~10 KB/day across 8 pairs.
The CSVs are the only copy of anything older than 180 days - they are the
dataset the funding-carry question needs.
"""
import csv, json, os, time, urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "data", "funding")
SYMBOLS = ["BTC", "ETH", "XRP", "LTC", "DOGE", "TRX", "ADA", "SOL"]
API = ("https://api.poloniex.com/v3/market/fundingRate/history"
       "?symbol={sym}&sT={sT}&eT={eT}&limit=100")   # ms, ms, <=100


def fetch(sym, days=7):
    now_ms = int(time.time() * 1000)
    rows, eT = {}, now_ms
    for _ in range(10):
        url = API.format(sym=sym, sT=now_ms - days * 86400000, eT=eT)
        with urllib.request.urlopen(url, timeout=20) as r:
            data = json.load(r).get("data") or []
        if not data:
            break
        for x in data:
            rows[int(x["fT"])] = float(x["fR"])
        oldest = min(int(x["fT"]) for x in data)
        if oldest >= eT:
            break
        eT = oldest - 1
        time.sleep(0.2)
    return rows


def merge(sym, new_rows):
    path = os.path.join(OUT, f"{sym}.csv")
    have = {}
    if os.path.exists(path):
        with open(path) as f:
            for row in csv.DictReader(f):
                have[int(row["settle_ms"])] = float(row["rate"])
    before = len(have)
    have.update(new_rows)
    tmp = path + ".tmp"
    with open(tmp, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["settle_ms", "settle_utc", "rate"])
        for t in sorted(have):
            utc = time.strftime("%Y-%m-%dT%H:%M:%S+00:00", time.gmtime(t / 1000))
            w.writerow([t, utc, f"{have[t]:.8f}"])
    os.replace(tmp, path)
    return len(have) - before


def main():
    os.makedirs(OUT, exist_ok=True)
    added_total = 0
    for c in SYMBOLS:
        sym = f"{c}_USDT_PERP"
        try:
            added = merge(sym, fetch(sym))
            added_total += added
            if added:
                print(f"  {sym}: +{added} settlements")
        except Exception as e:
            print(f"  {sym}: FAILED {type(e).__name__}: {e}")
    print(f"funding recorder: {added_total} new settlements")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
