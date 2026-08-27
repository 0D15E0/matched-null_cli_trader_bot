#!/usr/bin/env python3
"""Fetch Yahoo Finance INTRADAY candles into a CTC1 store the repo can read.

WHY THIS EXISTS AT ALL
----------------------
`cli_trader fetch --source yahoo` cannot store US-equity intraday bars. That is
not a bug in the fetcher - it is `CandleStore::append` correctly refusing bad
data. Yahoo stamps US hourly bars at :30 past the hour and emits stub bars on
early-close sessions (the half-day after Thanksgiving, Christmas Eve), so the
series contains gaps like 5400s on a 3600s grid. `CandleStore` rejects any gap
that is not a whole multiple of the period, because such a gap means the series
mixes timeframes or carries corrupt timestamps - and a store that silently glues
two grids together would poison every indicator downstream. Measured: 14 spacing
violations in a 2-year AAPL hourly series, and the append was refused outright.

THE FIX THIS SCRIPT APPLIES
---------------------------
Keep only the bars whose phase - `timestamp % period` - equals the dominant
phase in the response. Bars sharing one phase are, by construction, always an
integer number of periods apart, so whatever survives passes validation without
weakening it. On US hourly data this drops ~7 bars per 2-year series (the
early-close stubs and the occasional stray partial); on European and 5m/15m
data it usually drops nothing at all.

WHAT THAT COSTS YOU - read this before trusting a backtest on these stores:
  * The dropped bar's price action is not lost, but it is not resolved either:
    its move is absorbed into the close-to-close return of the gap it leaves.
    On an early-close session that means the half-session is folded into the
    next bar's return.
  * Yahoo publishes NO dividend adjustment for intraday data (no `adjclose` on
    these responses), so these series are price-return, NOT total-return. The
    daily stores fetched by `cli_trader` ARE adjusted. Do not compare an
    intraday backtest's absolute return against a daily one and conclude
    anything about timeframe.
  * Yahoo's intraday history is short and range-capped: roughly 730 days at 1h,
    and 60 days at 5m/15m/30m. There is no way to page further back.

THE LIMITATION THIS SCRIPT REPRESENTS
-------------------------------------
This is a Python side-channel for something the C++ fetcher should do itself.
Adding an opt-in phase filter to `YahooFinanceSource` (say `--phase-filter`,
defaulting off so the strict behaviour stays the default) would delete the need
for this file entirely and put intraday equity data behind the same validation,
repair and overlap-checking machinery as everything else. That change is not
made here deliberately: this directory records what the experiment actually ran,
and the experiment ran this.

FORMAT
------
Writes CTC1 exactly as specified in src/core/candle_store.h:
    magic "CTC1" (4 bytes) | periodSeconds (int64) | symbolLen (uint32)
    | symbol bytes | then fixed 48-byte records of
    int64 timestamp + 5 doubles (open, high, low, close, volume)
all little-endian. Verify a store you write with:
    cli_trader validate --symbol SYM --period P --data-dir DIR

USAGE
-----
    python3 yahoo_intraday.py --symbol AAPL --period 3600 --range 730d \\
        --out ../../data/AAPL_3600.ctc
"""

import argparse
import json
import os
import struct
import sys
import time
import urllib.error
import urllib.request

# Yahoo's interval strings for the periods CandleStore/cli_trader understand.
INTERVAL = {300: "5m", 900: "15m", 1800: "30m", 3600: "1h"}

# Yahoo refuses ranges longer than this per interval; asking for more silently
# returns the capped window, so state the real limit rather than pretend.
MAX_RANGE_DAYS = {300: 60, 900: 60, 1800: 60, 3600: 730}

RECORD = struct.Struct("<q5d")   # int64 ts + 5 doubles = 48 bytes


def fetch_rows(symbol, period, rng, timeout=60):
    """Return [(ts, open, high, low, close, volume)], ascending, nulls dropped."""
    url = ("https://query1.finance.yahoo.com/v8/finance/chart/" + symbol +
           "?range=" + rng + "&interval=" + INTERVAL[period])
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        payload = json.load(resp)

    chart = payload.get("chart") or {}
    if chart.get("error"):
        raise RuntimeError("Yahoo error for %s: %s" % (symbol, chart["error"]))
    results = chart.get("result") or []
    if not results:
        raise RuntimeError("Yahoo returned no result for %s "
                           "(check the ticker, e.g. 'ASML.AS' for Euronext)" % symbol)

    result = results[0]
    stamps = result.get("timestamp") or []
    quotes = (result.get("indicators") or {}).get("quote") or [{}]
    q = quotes[0]
    opens, highs = q.get("open") or [], q.get("high") or []
    lows, closes = q.get("low") or [], q.get("close") or []
    volumes = q.get("volume") or []

    # The OHLC arrays are meant to be index-parallel with `timestamp`, but
    # nothing in the wire format guarantees it - clamp to the shortest.
    n = min(len(stamps), len(opens), len(highs), len(lows), len(closes))
    rows = []
    for i in range(n):
        # Yahoo emits null rows for non-trading intervals inside the range.
        if None in (stamps[i], opens[i], highs[i], lows[i], closes[i]):
            continue
        vol = volumes[i] if i < len(volumes) and volumes[i] is not None else 0.0
        rows.append((int(stamps[i]), float(opens[i]), float(highs[i]),
                     float(lows[i]), float(closes[i]), float(vol)))
    rows.sort(key=lambda r: r[0])
    return rows


def drop_forming(rows, period, now=None):
    """Drop trailing bars whose window has not closed.

    CandleStore is append-only and later fetches filter on `timestamp <= last`,
    so a partial bar written once keeps its partial OHLCV forever.
    """
    now = time.time() if now is None else now
    while rows and rows[-1][0] + period > now:
        rows.pop()
    return rows


def phase_filter(rows, period):
    """Keep only the dominant `timestamp % period` phase. See module docstring."""
    if not rows:
        return rows, None, 0
    counts = {}
    for r in rows:
        counts[r[0] % period] = counts.get(r[0] % period, 0) + 1
    phase = max(counts, key=counts.get)
    kept = [r for r in rows if r[0] % period == phase]
    return kept, phase, len(rows) - len(kept)


def write_ctc(path, symbol, period, rows):
    """Write a CTC1 store atomically (temp file + rename), matching candle_store.h."""
    sym = symbol.encode("utf-8")
    tmp = path + ".tmp"
    parent = os.path.dirname(os.path.abspath(path))
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(tmp, "wb") as f:
        f.write(b"CTC1")
        f.write(struct.pack("<q", period))
        f.write(struct.pack("<I", len(sym)))
        f.write(sym)
        for r in rows:
            f.write(RECORD.pack(*r))
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Fetch Yahoo intraday candles into a CTC1 store, phase-filtered "
                    "so the result passes the repo's spacing validation.",
        epilog="Validate the result: cli_trader validate --symbol SYM --period P "
               "--data-dir DIR")
    ap.add_argument("--symbol", required=True,
                    help="Yahoo ticker, e.g. AAPL or ASML.AS")
    ap.add_argument("--period", required=True, type=int, choices=sorted(INTERVAL),
                    help="bar length in seconds (300, 900, 1800, 3600)")
    ap.add_argument("--range", default=None,
                    help="Yahoo range string, e.g. 60d or 730d. Defaults to this "
                         "interval's maximum.")
    ap.add_argument("--out", required=True, help="output .ctc path")
    ap.add_argument("--allow-forming", action="store_true",
                    help="keep the still-forming last bar (never do this for a "
                         "store you intend to keep - see module docstring)")
    args = ap.parse_args(argv)

    rng = args.range or ("%dd" % MAX_RANGE_DAYS[args.period])
    try:
        rows = fetch_rows(args.symbol, args.period, rng)
    except (urllib.error.URLError, RuntimeError, ValueError) as exc:
        print("fetch failed: %s" % exc, file=sys.stderr)
        return 1
    if not rows:
        print("no candles returned for %s" % args.symbol, file=sys.stderr)
        return 1

    before = len(rows)
    if not args.allow_forming:
        rows = drop_forming(rows, args.period)
    forming = before - len(rows)
    rows, phase, dropped = phase_filter(rows, args.period)
    if not rows:
        print("every bar was filtered out - refusing to write an empty store",
              file=sys.stderr)
        return 1

    write_ctc(args.out, args.symbol, args.period, rows)
    span_days = (rows[-1][0] - rows[0][0]) / 86400.0
    print("%s period=%d: %d bars, %.1f days (phase %ss, dropped %d off-grid, "
          "%d forming) -> %s"
          % (args.symbol, args.period, len(rows), span_days, phase, dropped,
             forming, args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
