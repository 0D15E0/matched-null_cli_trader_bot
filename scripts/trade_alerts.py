#!/usr/bin/env python3
"""Push a Telegram message the moment a sleeve buys or sells.

`notify.py` pages when the book BREAKS. This is the other half: a message when
the book DOES something — one per fill, with the size, the price, what it cost
and, on an exit, what the round trip made.

Design decisions, because the two obvious implementations are both wrong:

1. WATCH THE TRADE LOG; DO NOT NOTIFY FROM THE TRADING LOOP. The tempting
   place for this is `LiveTrader::placeBuy`, right where the fill is known. It
   is the worst place available: it puts a blocking HTTPS call to a third party
   inside the sleeve's critical section, between the fill and the state save.
   A Telegram outage would then stall — or, on an unhandled exception, kill —
   the process at the one instant it has bought coins and not yet written down
   that it owns them, which is the exact state the `halted-balance-mismatch`
   guard treats as an emergency. `<STATE_DIR>/<SYM>.trades.jsonl` already gets
   one JSON line per venue-confirmed fill, so tailing it carries the same
   information with no way to touch the book.

2. AN UNKNOWN SLEEVE STARTS SILENT. The cursor records how many lines of each
   trade log have already been announced. A sleeve with no cursor entry is
   initialised to the log's CURRENT length rather than to zero, so installing
   this — or losing `logs_pi/`, or restoring it from a backup — announces the
   NEXT fill instead of replaying months of history into the chat. The same
   rule covers a log that shrank, and the cursor is keyed by state directory
   so a paper/live switch re-initialises instead of reusing counts from the
   other book.

Delivery is at-least-once and order-preserving: the cursor advances past a
fill only once Telegram has accepted it, so a network blip delays a message
rather than dropping it, and fills from one sleeve always arrive in the order
they happened. A fill Telegram refuses MAX_SEND_FAILURES times in a row is
skipped with a log line — one poisoned record must not wedge every later fill
behind it forever.

Read-only, like everything else that touches Telegram here: this module reads
the trade logs and sends text. Nothing in it can trade, size or halt anything.

Usage:
    python3 scripts/trade_alerts.py              # scan and send (what the bot calls)
    python3 scripts/trade_alerts.py --dry-run    # print what would be sent
    python3 scripts/trade_alerts.py --test       # re-send the newest real fill
    python3 scripts/trade_alerts.py --status     # cursor vs. logs on disk
    python3 scripts/trade_alerts.py --reset      # adopt current lengths, announce nothing
"""
import fcntl, glob, json, os, re, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
if HERE not in sys.path:                      # so `import notify` works when
    sys.path.insert(0, HERE)                  # imported from the bot process

import notify                                 # noqa: E402  (path set above)

BOT_ENV = os.path.join(REPO, "deploy", "pi", "bot.env")
CURSOR = os.path.join(REPO, "logs_pi", ".trade_alert_state.json")

# Eight sleeves cannot produce more than eight fills in one cycle, so this cap
# is a runaway guard, not a policy. Anything above it simply waits for the next
# scan (~30 s later) instead of being dropped.
MAX_PER_SCAN = 10
SEND_GAP = 0.4              # seconds between messages; Telegram throttles ~1/s
MAX_SEND_FAILURES = 5       # give up on ONE fill after this many failed scans
MAX_MSG = 3800              # Telegram's limit is 4096; leave room for the fence
# Wall-clock ceiling for one scan. This runs INSIDE the query bot's poll loop,
# so a Telegram endpoint that hangs rather than refuses would otherwise stall
# /report for 8 sleeves x 2 attempts x a 15 s socket timeout = four minutes.
# Whatever does not fit in the budget simply goes out on the next scan.
SCAN_BUDGET = 45.0


# ---- configuration ------------------------------------------------------

def _bot_env():
    """The same one-file configuration every sleeve reads. Parsed the same way
    check_pi.py parses it, so the alerter can never disagree with the health
    check about which book is running."""
    cfg = {}
    try:
        for line in open(BOT_ENV):
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip()
    except OSError:
        pass
    return cfg


def state_dir(cfg=None):
    cfg = _bot_env() if cfg is None else cfg
    return os.path.join(REPO, cfg.get("STATE_DIR", "state_paper"))


# ---- cursor -------------------------------------------------------------

def _load_cursor():
    try:
        c = json.load(open(CURSOR))
        if isinstance(c, dict):
            return c
    except Exception:
        pass
    return {}


def _save_cursor(cur):
    """Atomic, and never fatal: losing the cursor costs one silent
    re-initialisation, not a wrong message."""
    try:
        os.makedirs(os.path.dirname(CURSOR), exist_ok=True)
        tmp = CURSOR + ".tmp"
        with open(tmp, "w") as fh:
            json.dump(cur, fh)
        os.replace(tmp, CURSOR)
    except OSError as e:
        print(f"[trade_alerts] could not save cursor: {type(e).__name__}")


def _lock():
    """A scan holds this for its duration, or does not run.

    Two scanners racing would each read the same cursor and each announce the
    same fills. That is not hypothetical: the bot scans every poll, and running
    `scripts/trade_alerts.py` by hand on a live box is an obvious thing to do
    while debugging. flock is released by the kernel when the process exits, so
    a killed scan cannot leave a stale lock behind.

    Returns the open file object (keep it alive), or None if someone else has it.
    """
    try:
        lock = CURSOR + ".lock"          # derived, so it follows CURSOR
        os.makedirs(os.path.dirname(lock), exist_ok=True)
        fh = open(lock, "w")
        fcntl.flock(fh.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return fh
    except OSError:
        return None


# ---- reading the trade logs --------------------------------------------

def _read_fills(path):
    """(records, consumable_count), indexed by line number.

    A final line that does not parse is assumed to be a fill being appended
    right now rather than a corrupt one, so it is NOT counted and the next
    scan sees it whole. A bad line anywhere else is counted (so it cannot
    block the log) but never announced.
    """
    try:
        # errors="replace": a byte the locale cannot decode must degrade to one
        # unannounceable line, not to a UnicodeDecodeError - that escapes the
        # OSError catch, unwinds the whole scan, and silences all eight sleeves
        # for as long as the bad byte stays in the file.
        with open(path, "r", errors="replace") as fh:
            lines = fh.read().splitlines()
    except OSError:
        return [], 0
    recs = []
    for i, line in enumerate(lines):
        line = line.strip()
        if not line:
            recs.append(None)
            continue
        try:
            recs.append(json.loads(line))
        except ValueError:
            if i == len(lines) - 1:
                return recs, len(recs)        # torn tail: stop just before it
            recs.append(None)
    return recs, len(recs)


# ---- formatting ---------------------------------------------------------

def _f(v, default=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def _short(sym):
    return sym.replace("_USDT", "")


def _price(p):
    """A price column that works for both BTC at 79,000 and TRX at 0.09."""
    if p >= 1000:
        return f"${p:,.2f}"
    if p >= 1:
        return f"${p:,.4f}"
    return "$" + (f"{p:.8f}".rstrip("0").rstrip(".") or "0")


def _amount(a):
    s = f"{a:,.8f}".rstrip("0").rstrip(".")
    return s or "0"


def _money(v):
    return f"${v:,.2f}"


def _signed_money(v):
    return f"{'+' if v >= 0 else '-'}${abs(v):,.2f}"


def _partial(rec):
    """Whether the venue said this order did not fully fill.

    Best effort by design: `note` holds the raw exchange response, and digging
    the order state out of it with a regex is cheaper and safer than parsing a
    two-blob string. A note that is missing or shaped differently simply means
    the message says nothing about partiality.
    """
    try:
        m = re.search(r'"state"\s*:\s*"([A-Z_]+)"', str(rec.get("note", "")))
        return bool(m) and m.group(1) != "FILLED"
    except Exception:
        return False


def book_line(sdir):
    """One line of context under the fill: what the book looks like now.

    Read at scan time, i.e. after the sleeve has saved the state the fill
    produced — so an entry message shows the position it just opened.
    """
    longs = total = 0
    value = realized = 0.0
    for f in sorted(glob.glob(os.path.join(sdir, "*.state.json"))):
        try:
            s = json.load(open(f))
        except Exception:
            continue
        total += 1
        v = _f(s.get("base_balance")) * _f(s.get("last_price"))
        if v > 1.0:
            longs += 1
            value += v
        realized += _f(s.get("realized_pnl"))
    if not total:
        return ""
    # Whole dollars, matching the /report footer: the cents on an aggregate
    # are noise next to a $487 book.
    return (f"{longs}/{total} long · positions ${value:,.0f}"
            f" · realized {_signed_money(realized)}")


def format_fill(rec, mode="?", host="?", book="", suffix=""):
    sym = _short(str(rec.get("symbol", "?")))
    side = str(rec.get("side", "")).lower()
    price, amount = _f(rec.get("price")), _f(rec.get("amount"))
    quote = _f(rec.get("quote_value")) or price * amount
    fee = _f(rec.get("fee"))

    stamp = ""
    try:
        stamp = " " + time.strftime("%H:%M", time.localtime(int(rec["time"])))
    except Exception:
        pass                                  # a fill with no usable timestamp

    if side == "buy":
        head = f"🟢 *BOUGHT {sym}*"
    elif side == "sell":
        head = f"🔴 *SOLD {sym}*"
    else:
        head = f"• *{side or '?'} {sym}*"
    if _partial(rec):
        head += " _(partial)_"

    body = [f"{_amount(amount)} {sym} @ {_price(price)}",
            f"value {_money(quote)} · fee {_money(fee)}"]
    if rec.get("pnl") is not None:
        pnl = _f(rec.get("pnl"))
        # What the position cost to open: proceeds (value net of the exit fee)
        # minus what the round trip made. The live side measures P&L against
        # total outlay including the entry fee, so this recovers that basis
        # rather than re-deriving one from the entry price.
        cost = quote - fee - pnl
        pct = f"  ({pnl / cost * 100:+.1f}%)" if cost > 0 else ""
        body.append(f"P&L {_signed_money(pnl)}{pct}")

    msg = (f"{head} — `{host}` ({mode}){stamp}{suffix}\n"
           "```\n" + "\n".join(body) + "\n```")
    return msg + ("\n" + book if book else "")


def _strip_markdown(text):
    return re.sub(r"[*`_\[\]]", "", text)


def _send(text):
    """notify.send, with one plain-text retry.

    Telegram rejects an entire message whose Markdown does not parse. Without
    the retry a single such fill would fail forever and hold up every fill
    behind it; the unformatted version still says what happened.
    """
    if len(text) > MAX_MSG:                   # notify.send has no guard of its own
        text = text[:MAX_MSG] + "\n… (truncated)"
    if notify.send(text):
        return True
    return notify.send(_strip_markdown(text), markdown=False)


# ---- the scan -----------------------------------------------------------

def scan(dry_run=False, sender=None):
    """Announce every fill that appeared since the last scan.

    Returns the number of messages delivered. Never raises: this runs inside
    the query bot's poll loop, and an alerter that throws must not be able to
    stop the bot from answering /report.
    """
    fh = _lock()
    if fh is None:
        print("[trade_alerts] another scan is already running; skipping this one")
        return 0
    try:
        return _scan(dry_run, sender or _send)
    except Exception as e:
        print(f"[trade_alerts] scan failed: {type(e).__name__}: {e}")
        return 0
    finally:
        fh.close()


def _scan(dry_run, send):
    cfg = _bot_env()
    sdir = state_dir(cfg)
    mode, host = cfg.get("MODE", "?"), os.uname().nodename

    cur = _load_cursor()
    if cur.get("dir") != sdir:
        # A paper/live switch. Line counts from the other book mean nothing
        # here, so drop them and re-adopt below rather than replay or skip.
        cur = {"dir": sdir}
    seen = cur.setdefault("lines", {})
    fails = cur.setdefault("fails", {})

    ctx = {"book": None, "sent": 0, "deadline": time.monotonic() + SCAN_BUDGET}
    for path in sorted(glob.glob(os.path.join(sdir, "*.trades.jsonl"))):
        if _scan_done(ctx):
            break
        sym = os.path.basename(path)[: -len(".trades.jsonl")]
        try:
            _scan_symbol(sym, path, cur, seen, fails, sdir, mode, host, dry_run,
                         send, ctx)
        except Exception as e:
            # One sleeve's unreadable log or unexpected record shape must not
            # cost the other seven their alerts. Without this, the blanket catch
            # in scan() unwinds every remaining symbol - and does so again on
            # every scan for as long as the cause persists, which turns one bad
            # file into a silent book.
            print(f"[trade_alerts] {sym}: skipped this scan "
                  f"({type(e).__name__}: {e})")
    _save_cursor(cur)
    return ctx["sent"]


def _scan_done(ctx):
    """Stop conditions shared by both loops. Neither drops a fill: whatever is
    left simply keeps its place in the cursor and goes out on the next scan."""
    return ctx["sent"] >= MAX_PER_SCAN or time.monotonic() > ctx["deadline"]


def _scan_symbol(sym, path, cur, seen, fails, sdir, mode, host, dry_run, send, ctx):
    recs, count = _read_fills(path)
    prev = seen.get(sym)
    if not isinstance(prev, int) or prev > count:
        seen[sym] = count                     # first sight, or the log shrank
        fails.pop(sym, None)
        return

    for i in range(prev, count):
        if _scan_done(ctx):
            return
        rec = recs[i]
        if not isinstance(rec, dict):
            seen[sym] = i + 1                 # blank or corrupt line: nothing to say
            continue
        if ctx["book"] is None:               # read the book once per scan, and
            ctx["book"] = book_line(sdir)     # only if there is something to send
        text = format_fill(rec, mode, host, ctx["book"])
        if dry_run:
            print(f"[trade_alerts] DRY RUN, would send:\n{text}\n")
            seen[sym] = i + 1
            ctx["sent"] += 1
            continue
        try:
            delivered = send(text)
        except Exception as e:
            # A sender that RAISES must be a retry, not an abort: unwinding here
            # would skip the cursor save and re-announce every fill this scan
            # had already delivered.
            print(f"[trade_alerts] {sym}: sender raised {type(e).__name__}; will retry")
            delivered = False
        if delivered:
            seen[sym] = i + 1
            fails.pop(sym, None)
            ctx["sent"] += 1
            # Saved per message, not once at the end: a restart between two
            # fills must not re-announce the first one.
            _save_cursor(cur)
            time.sleep(SEND_GAP)
        else:
            n = fails.get(sym, 0) + 1
            fails[sym] = n
            if n >= MAX_SEND_FAILURES:
                print(f"[trade_alerts] {sym}: giving up on one fill after "
                      f"{n} failed attempts; skipping it")
                seen[sym] = i + 1
                fails.pop(sym, None)
            return                            # keep this sleeve's fills in order


# ---- command line -------------------------------------------------------

def _newest_fill(sdir):
    best = None
    for path in glob.glob(os.path.join(sdir, "*.trades.jsonl")):
        recs, count = _read_fills(path)
        for rec in recs[:count]:
            if isinstance(rec, dict) and (best is None
                                          or _f(rec.get("time")) > _f(best.get("time"))):
                best = rec
    return best


def main(argv):
    cfg = _bot_env()
    sdir = state_dir(cfg)

    if "--status" in argv:
        cur = _load_cursor()
        print(f"state dir : {sdir}")
        print(f"cursor    : {CURSOR}")
        print(f"cursor dir: {cur.get('dir', '(none)')}")
        print(f"telegram  : {'configured' if notify.configured() else 'NOT configured'}")
        for path in sorted(glob.glob(os.path.join(sdir, "*.trades.jsonl"))):
            sym = os.path.basename(path)[: -len(".trades.jsonl")]
            _, count = _read_fills(path)
            done = cur.get("lines", {}).get(sym, "-")
            print(f"  {sym:<10} announced {done}/{count}")
        return 0

    if "--reset" in argv:
        # Under the lock: this is the one command an operator runs by hand on a
        # live box, and the bot is scanning every 30 s. Without it the two racing
        # writers can lose the reset (replaying fills) or lose the bot's cursor
        # advance (skipping one).
        fh = _lock()
        if fh is None:
            print("another scan is running; try again in a moment")
            return 1
        try:
            cur = {"dir": sdir, "lines": {}, "fails": {}}
            for path in sorted(glob.glob(os.path.join(sdir, "*.trades.jsonl"))):
                sym = os.path.basename(path)[: -len(".trades.jsonl")]
                _, count = _read_fills(path)
                cur["lines"][sym] = count
            _save_cursor(cur)
        finally:
            fh.close()
        print(f"cursor reset to current lengths; {len(cur['lines'])} sleeve(s) adopted")
        return 0

    if "--test" in argv:
        # End-to-end proof without waiting for a real trade: re-send the most
        # recent fill, marked as a test so it is never mistaken for a new one.
        rec = _newest_fill(sdir) or {"symbol": "BTC_USDT", "side": "buy", "price": 79070.66,
                                     "amount": 0.00263, "quote_value": 207.96, "fee": 0.26,
                                     "time": int(time.time())}
        text = format_fill(rec, cfg.get("MODE", "?"), os.uname().nodename,
                           book_line(sdir), suffix="  ·  _test_")
        ok = _send(text)
        print("delivered" if ok else "NOT delivered")
        return 0 if ok else 1

    n = scan(dry_run="--dry-run" in argv)
    print(f"{n} fill(s) announced")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
