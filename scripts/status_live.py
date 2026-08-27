#!/usr/bin/env python3
"""One-screen daily status of the live book: per-sleeve position, action,
prices and P&L, plus book totals. Read-only - just formats state_live/.

  python3 scripts/status_live.py            # the table
  python3 scripts/status_live.py --trades   # ...plus each sleeve's last fill
"""
import argparse, glob, json, os, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def age(ts):
    if not ts: return "-"
    m = (time.time() - ts) / 60
    if m < 90: return f"{m:.0f}m"
    if m < 48 * 60: return f"{m/60:.0f}h"
    return f"{m/1440:.0f}d"

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--state-dir", default=os.path.join(REPO, "state_live"))
    ap.add_argument("--trades", action="store_true", help="show each sleeve's last fill")
    a = ap.parse_args()

    files = sorted(glob.glob(os.path.join(a.state_dir, "*.state.json")))
    if not files:
        print(f"no state files in {a.state_dir}"); return 1

    rows, cash, pos_val, upnl_tot, rpnl_tot = [], 0.0, 0.0, 0.0, 0.0
    for f in files:
        s = json.load(open(f))
        sym = s["symbol"].replace("_USDT", "")
        px, base = s.get("last_price", 0.0), s.get("base_balance", 0.0)
        in_pos = base * px > 1.0                      # above dust
        entry = s.get("entry_price", 0.0)
        outlay = s.get("entry_outlay", 0.0)
        value = base * px
        upnl = (value - outlay) if in_pos and outlay > 0 else 0.0
        upnl_pct = (px / entry - 1) * 100 if in_pos and entry > 0 else 0.0
        cash += s.get("quote_balance", 0.0)
        pos_val += value; upnl_tot += upnl
        rpnl = s.get("realized_pnl", 0.0); rpnl_tot += rpnl
        state = "LONG" if in_pos else "flat"
        act = s.get("last_action", "?")
        flag = ""
        if act.startswith("halted"): flag = "HALTED"
        elif s.get("last_error"): flag = "ERR"
        if (time.time() - s.get("last_poll_at", 0)) > 900: flag = (flag + " STALE").strip()
        rows.append((sym, state, act, value, entry, px, upnl, upnl_pct, rpnl,
                     f"{s.get('wins',0)}-{s.get('losses',0)}",
                     age(s.get("last_signal_at", 0)), flag, s))

    print(f"{'sleeve':<6} {'pos':<5} {'last action':<22} {'value$':>8} {'entry':>11} "
          f"{'now':>11} {'uP&L$':>8} {'uP&L%':>7} {'rP&L$':>7} {'W-L':>5} {'signal':>7}  flags")
    print("-" * 118)
    for r in rows:
        print(f"{r[0]:<6} {r[1]:<5} {r[2][:21]:<22} {r[3]:>8.2f} "
              f"{(f'{r[4]:,.4g}' if r[4] else '-'):>11} {f'{r[5]:,.4g}':>11} "
              f"{r[6]:>+8.2f} {r[7]:>+6.1f}% {r[8]:>+7.2f} {r[9]:>5} {r[10]:>7}  {r[11]}")
    print("-" * 118)
    # Each sleeve's quote_balance is min(shared pool, its cap), so summing
    # them double-counts the pool. Real cash needs one venue call:
    #   ./build/cli_trader balances
    print(f"positions ${pos_val:,.2f}  |  unrealized {upnl_tot:+,.2f}  "
          f"realized {rpnl_tot:+,.2f}  |  cash: run ./build/cli_trader balances "
          f"(sleeve claims overlap under caps)")
    print("\nlegend: pos LONG/flat = actual holding | last action: hold=no change this cycle,")
    print("  buy/sell=order this cycle, scanning=flat & watching, halted-*=trading suspended")
    print("  (see runbook 3) | signal = time since last entry/exit signal")

    if a.trades:
        print()
        for r in rows:
            tl = os.path.join(a.state_dir, r[0] + "_USDT.trades.jsonl")
            if not os.path.exists(tl): continue
            last = None
            with open(tl) as fh:
                for line in fh: last = line
            if last:
                t = json.loads(last)
                print(f"  {r[0]:<6} last fill: {t['side']} {t['amount']:.6g} @ {t['price']:,.6g} "
                      f"(~{t['quote_value']:,.2f} USDT, fee {t['fee']:.4f}) {age(t['time'])} ago")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
