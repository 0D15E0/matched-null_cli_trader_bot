#!/usr/bin/env python3
"""Fetch Deribit's DVOL implied-volatility index history.

DVOL is Deribit's model-free 30-day implied volatility index for BTC (and ETH)
- the crypto analogue of VIX, computed from the whole option surface rather than
from one contract. It is quoted in annualized percent.

Why this specific series: the encompassing regression needs THE MARKET'S
forecast of realized volatility over a fixed horizon, and DVOL is exactly that
at a 30-day horizon, published as a single number with a long history and no
strike/expiry selection choices for us to make (and therefore no room for us to
tune anything).

Endpoint (public, no auth):
  /api/v2/public/get_volatility_index_data?currency=BTC
      &start_timestamp=<ms>&end_timestamp=<ms>&resolution=<secs|1D>
returning [[ts_ms, open, high, low, close], ...].

Output: CSV  date,timestamp,dvol_close  (one row per UTC day)
"""
import argparse
import csv
import json
import sys
import time
import urllib.request
from datetime import datetime, timezone

URL = ("https://www.deribit.com/api/v2/public/get_volatility_index_data"
       "?currency={cur}&start_timestamp={a}&end_timestamp={b}&resolution={res}")


def fetch_window(cur, a_ms, b_ms, res, timeout=60):
    req = urllib.request.Request(URL.format(cur=cur, a=a_ms, b=b_ms, res=res),
                                 headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        payload = json.load(r)
    if "error" in payload:
        raise RuntimeError("Deribit error: %s" % payload["error"])
    return (payload.get("result") or {}).get("data") or []


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--currency", default="BTC")
    ap.add_argument("--start", default="2021-01-01", help="UTC date, YYYY-MM-DD")
    ap.add_argument("--resolution", default="43200",
                    help="seconds (60/3600/43200) or 1D; 43200 = 12h, then "
                         "reduced to one row per UTC day")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    start = int(datetime.strptime(args.start, "%Y-%m-%d")
                .replace(tzinfo=timezone.utc).timestamp() * 1000)
    now = int(time.time() * 1000)

    rows = []
    # Page forward in 90-day chunks: the endpoint silently caps the number of
    # points it will return, and a single multi-year request comes back
    # truncated rather than erroring.
    CHUNK = 90 * 86400 * 1000
    a = start
    while a < now:
        b = min(a + CHUNK, now)
        try:
            data = fetch_window(args.currency, a, b, args.resolution)
        except Exception as exc:
            print("chunk %s failed: %s" % (a, exc), file=sys.stderr)
            data = []
        rows.extend(data)
        a = b
        time.sleep(0.25)

    # Reduce to the LAST observation of each UTC day: the regression is daily
    # and a day's closing implied vol is the natural once-per-day reading.
    by_day = {}
    for rec in rows:
        ts_ms, _o, _h, _l, close = rec[0], rec[1], rec[2], rec[3], rec[4]
        d = datetime.fromtimestamp(ts_ms / 1000, timezone.utc).strftime("%Y-%m-%d")
        prev = by_day.get(d)
        if prev is None or ts_ms > prev[0]:
            by_day[d] = (ts_ms, close)

    with open(args.out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["date", "timestamp", "dvol_close"])
        for d in sorted(by_day):
            ts_ms, close = by_day[d]
            w.writerow([d, ts_ms // 1000, "%.4f" % close])

    if by_day:
        ks = sorted(by_day)
        print("%d daily DVOL observations, %s .. %s" % (len(by_day), ks[0], ks[-1]))
    else:
        print("no data returned", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
