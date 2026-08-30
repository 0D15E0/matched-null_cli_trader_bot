#!/usr/bin/env python3
"""Read-only Telegram query bot — and fill alerter — for the trading book.

Two jobs, one process, one Telegram token:

  * ANSWERS the read-only commands below when you ask.
  * ANNOUNCES, unprompted, every buy and sell the book makes. Between long
    polls it hands off to `trade_alerts.scan()`, which tails the sleeves'
    trade logs and pushes one message per fill. The alerter lives here rather
    than in its own unit because this process already runs continuously, is
    already restarted forever by systemd, and already owns the Telegram token
    — and because the poll loop's 30 s timeout is also the worst-case delay
    between a fill and the message about it. It is deliberately NOT wired
    into the C++ trading loop; see the header of scripts/trade_alerts.py for
    why that would be dangerous.

    /report    positions + health + cash, formatted for a phone
    /status    the sleeve table
    /health    run the health check
    /balances  real account holdings from the venue
    /log SYM   last journal lines for one sleeve
    /help

SECURITY, deliberately narrow:

  * READ-ONLY. There is no command that trades, stops a sleeve, moves money or
    edits config. A leaked token therefore exposes information, not the
    account. Anything that changes state stays on SSH, behind a key.
  * WHITELIST. Only CHAT_ID from .env is answered; every other chat is ignored
    and logged. Telegram bots reply to anyone who finds them, so without this
    a stranger messaging the bot would get the portfolio.
  * NO SHELL. Commands are fixed argv lists run without shell=True, and the
    only user-supplied value (a symbol for /log) is checked against the known
    sleeve list rather than interpolated.
  * Outbound only. Long polling means no inbound port, no webhook, nothing
    exposed to the internet.

Long polling starts from offset=-1 so a restart skips the backlog instead of
replaying old commands.
"""
import json, os, re, subprocess, sys, time, urllib.parse, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
if HERE not in sys.path:
    sys.path.insert(0, HERE)
SYMBOLS = ["BTC_USDT", "ETH_USDT", "XRP_USDT", "LTC_USDT",
           "DOGE_USDT", "TRX_USDT", "ADA_USDT", "SOL_USDT"]
MAX_MSG = 3800          # Telegram hard limit is 4096; leave room for fences
POLL_TIMEOUT = 30


def env_get(key):
    try:
        m = re.search(rf'^\s*(?:export\s+)?{key}\s*=\s*["\']?([^"\'\n#]+)',
                      open(os.path.join(REPO, ".env")).read(), re.M)
        return m.group(1).strip() if m else None
    except OSError:
        return None


TOKEN = env_get("TELEGRAM_BOT_TOKEN")
CHAT_ID = env_get("TELEGRAM_CHAT_ID")


def api(method, params=None, timeout=40):
    url = f"https://api.telegram.org/bot{TOKEN}/{method}"
    data = urllib.parse.urlencode(params or {}).encode()
    with urllib.request.urlopen(urllib.request.Request(url, data=data), timeout=timeout) as r:
        return json.load(r)


def send(text, markdown=True):
    if len(text) > MAX_MSG:
        text = text[:MAX_MSG] + "\n… (truncated)"
    p = {"chat_id": CHAT_ID, "text": text, "disable_web_page_preview": "true"}
    if markdown:
        p["parse_mode"] = "Markdown"
    try:
        api("sendMessage", p, timeout=20)
    except Exception as e:
        print(f"[bot] send failed: {type(e).__name__}")


def run(argv, timeout=60):
    """Fixed argv, never a shell string."""
    try:
        r = subprocess.run(argv, cwd=REPO, capture_output=True, text=True, timeout=timeout)
        return (r.stdout or "") + (r.stderr or ""), r.returncode
    except subprocess.TimeoutExpired:
        return "(timed out)", 124
    except Exception as e:
        return f"({type(e).__name__})", 1


def push_fills():
    """Announce any buy or sell that happened since the last poll.

    Imported lazily and wrapped completely, for the same reason check_pi.py
    wraps its notifier: a missing or broken alerter must degrade to a log line,
    never stop the bot answering /report. `scan()` already swallows its own
    errors; this catches the import itself and anything it cannot.
    """
    try:
        import trade_alerts
        trade_alerts.scan()
    except Exception as e:
        print(f"[bot] fill alerts unavailable: {type(e).__name__}")


def mode():
    try:
        m = re.search(r"^MODE=(\w+)", open(os.path.join(REPO, "deploy/pi/bot.env")).read(), re.M)
        return m.group(1) if m else "?"
    except OSError:
        return "?"


def cmd_report():
    """Phone-shaped: the wide terminal table is unreadable on a handset."""
    import glob
    rows, pos_val, upnl, rpnl, longs = [], 0.0, 0.0, 0.0, 0
    for f in sorted(glob.glob(os.path.join(REPO, "state_live", "*.state.json"))):
        try:
            s = json.load(open(f))
        except Exception:
            continue
        sym = s["symbol"].replace("_USDT", "")
        val = s.get("base_balance", 0) * s.get("last_price", 0)
        held = val > 1.0
        if held:
            longs += 1
            pos_val += val
            ent = s.get("entry_price", 0)
            pct = (s["last_price"] / ent - 1) * 100 if ent else 0.0
            upnl += val - s.get("entry_outlay", 0)
            rows.append(f"{sym:<5} LONG {val:>7.0f}$ {pct:>+5.1f}%")
        else:
            rows.append(f"{sym:<5} flat        -")
        rpnl += s.get("realized_pnl", 0.0)
        if str(s.get("last_action", "")).startswith("halted"):
            rows[-1] += "  HALT"

    health, code = run([sys.executable, "scripts/check_pi.py"])
    ok = code == 0
    bal, _ = run([os.path.join(REPO, "build/cli_trader"), "balances"], timeout=45)
    cash = ""
    m = re.search(r"^SPOT\s+USDT\s+\S+\s+\S+\s+([\d.]+)", bal, re.M)
    if m:
        cash = f"  cash ${float(m.group(1)):,.0f}"
    tot = re.search(r"Estimated total:\s*~([\d.]+)", bal)

    head = f"*Book* — `{os.uname().nodename}` ({mode()})  {time.strftime('%H:%M')}"
    body = "\n".join(rows)
    foot = (f"\n{longs}/8 long · positions ${pos_val:,.0f}{cash}"
            f"\nunreal {upnl:+,.2f} · real {rpnl:+,.2f}")
    if tot:
        foot += f"\naccount ~${float(tot.group(1)):,.0f}"
    status = "✅ HEALTHY" if ok else "⚠️ UNHEALTHY\n" + health.strip()
    return f"{head}\n```\n{body}\n```{foot}\n\n{status}"


def cmd_status():
    out, _ = run([sys.executable, "scripts/status_live.py"])
    return "```\n" + out.strip()[:MAX_MSG - 20] + "\n```"


def cmd_health():
    out, code = run([sys.executable, "scripts/check_pi.py"])
    return ("✅ *HEALTHY* — all checks passed" if code == 0
            else "⚠️ *UNHEALTHY*\n```\n" + out.strip() + "\n```")


def cmd_balances():
    out, _ = run([os.path.join(REPO, "build/cli_trader"), "balances"], timeout=45)
    lines = [l for l in out.splitlines() if l.startswith("SPOT") or "Estimated" in l]
    return "*Account holdings*\n```\n" + "\n".join(lines)[:MAX_MSG - 40] + "\n```"


def cmd_log(arg):
    sym = (arg or "").upper()
    if not sym.endswith("_USDT"):
        sym += "_USDT"
    if sym not in SYMBOLS:                       # whitelist, never interpolate
        return "Unknown sleeve. Use one of: " + ", ".join(s.replace("_USDT", "") for s in SYMBOLS)
    out, _ = run(["journalctl", "-u", f"cli-trader@{sym}", "-n", "25", "--no-pager"])
    return f"*{sym}*\n```\n" + out.strip()[-(MAX_MSG - 60):] + "\n```"


HELP = ("*Commands* (read-only)\n"
        "/report — positions, cash and health\n"
        "/status — the sleeve table\n"
        "/health — run the health check\n"
        "/balances — account holdings\n"
        "/log BTC — recent journal for one sleeve\n\n"
        "Fills are pushed automatically: one message per buy and sell.\n"
        "_Nothing here can trade, stop a sleeve or move money._")


def handle(text):
    parts = text.strip().split()
    if not parts:
        return None
    cmd = parts[0].lower().split("@")[0]          # /report@BotName -> /report
    arg = parts[1] if len(parts) > 1 else None
    if cmd == "/log":
        return cmd_log(arg)
    simple = {
        "/report": cmd_report,
        "/status": cmd_status,
        "/health": cmd_health,
        "/balances": cmd_balances,
        "/start": lambda: HELP,
        "/help": lambda: HELP,
    }
    fn = simple.get(cmd)
    return fn() if fn else None


def main():
    # systemd captures stdout into a file, not a tty, so Python block-buffers
    # it and logs_pi/telegram.log sits at 0 bytes for days while the unit runs
    # perfectly (observed 2026-08-30, four days after install). That log is the
    # only window onto the fill alerter, so make it line-buffered here rather
    # than depending on a PYTHONUNBUFFERED= in the unit file.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(line_buffering=True)
        except Exception:
            pass

    if not TOKEN or not CHAT_ID:
        print("TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID missing from .env"); return 1
    print(f"[bot] polling; answering chat {CHAT_ID} only; pushing fills")
    offset = None
    try:                                    # skip backlog on restart
        r = api("getUpdates", {"offset": -1}, timeout=20).get("result", [])
        if r:
            offset = r[-1]["update_id"] + 1
    except Exception:
        pass
    while True:
        push_fills()
        try:
            p = {"timeout": POLL_TIMEOUT}
            if offset is not None:
                p["offset"] = offset
            for u in api("getUpdates", p, timeout=POLL_TIMEOUT + 15).get("result", []):
                offset = u["update_id"] + 1
                msg = u.get("message") or u.get("edited_message") or {}
                chat = str((msg.get("chat") or {}).get("id", ""))
                text = msg.get("text") or ""
                if chat != str(CHAT_ID):
                    print(f"[bot] ignoring chat {chat}")   # not the owner
                    continue
                if not text.startswith("/"):
                    continue
                print(f"[bot] {text.split()[0]}")
                try:
                    reply = handle(text)
                except Exception as e:
                    reply = f"command failed: {type(e).__name__}"
                send(reply or "Unknown command. /help")
        except Exception as e:
            print(f"[bot] poll error: {type(e).__name__}; retrying in 15s")
            time.sleep(15)


if __name__ == "__main__":
    sys.exit(main())
