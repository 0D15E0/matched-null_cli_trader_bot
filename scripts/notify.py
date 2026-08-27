#!/usr/bin/env python3
"""Telegram push notifications for the trading book.

Design decisions, because the obvious implementation is wrong in two ways:

1. ALERT ON TRANSITIONS, NOT ON STATE. The health check runs every 10 minutes.
   A sleeve that halts at 03:00 and is noticed at 09:00 would otherwise have
   sent 36 identical messages, and the 37th is the one you ignore. This sends
   one message when the book BREAKS, one when it RECOVERS, and a reminder only
   every `REPEAT_HOURS` while it stays broken.

2. NOTIFICATION FAILURE MUST NEVER BREAK THE HEALTH CHECK. Every call is
   wrapped; a dead network, a revoked token or a Telegram outage degrades to a
   log line. The health check's exit code is decided by the book's health, not
   by whether a message got delivered.

Credentials live in .env next to the binary (chmod 600, gitignored) as
TELEGRAM_BOT_TOKEN and TELEGRAM_CHAT_ID. The token is never logged or echoed:
errors print the HTTP status only.

Read-only by design. This module can SEND; nothing here can receive commands,
so a leaked token exposes notification text and nothing else - it cannot trade,
stop a sleeve, or read the account.
"""
import json, os, re, time, urllib.request, urllib.parse, urllib.error

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATE_FILE = os.path.join(REPO, "logs_pi", ".alert_state.json")
REPEAT_HOURS = 6          # re-remind while still broken
API = "https://api.telegram.org/bot{token}/sendMessage"


def _creds():
    """(token, chat_id) from .env, or (None, None) if not configured."""
    path = os.path.join(REPO, ".env")
    try:
        text = open(path).read()
    except OSError:
        return None, None
    def find(key):
        m = re.search(rf'^\s*(?:export\s+)?{key}\s*=\s*["\']?([^"\'\n#]+)', text, re.M)
        return m.group(1).strip() if m else None
    return find("TELEGRAM_BOT_TOKEN"), find("TELEGRAM_CHAT_ID")


def configured():
    t, c = _creds()
    return bool(t and c)


def send(text, dry_run=False):
    """Send one message. Returns True on success. Never raises."""
    token, chat = _creds()
    if not token or not chat:
        print("[notify] TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID not in .env - skipping")
        return False
    if dry_run:
        print("[notify] DRY RUN, would send:\n" + text)
        return True
    data = urllib.parse.urlencode({
        "chat_id": chat,
        "text": text,
        "parse_mode": "Markdown",
        "disable_web_page_preview": "true",
    }).encode()
    try:
        req = urllib.request.Request(API.format(token=token), data=data)
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.load(r).get("ok", False)
    except urllib.error.HTTPError as e:
        # Deliberately does not print the URL: it contains the token.
        print(f"[notify] Telegram HTTP {e.code} - message not delivered")
    except Exception as e:
        print(f"[notify] send failed: {type(e).__name__} - message not delivered")
    return False


def _load_state():
    try:
        return json.load(open(STATE_FILE))
    except Exception:
        return {"healthy": True, "last_alert": 0, "signature": ""}


def _save_state(st):
    try:
        os.makedirs(os.path.dirname(STATE_FILE), exist_ok=True)
        tmp = STATE_FILE + ".tmp"
        with open(tmp, "w") as fh:
            json.dump(st, fh)
        os.replace(tmp, STATE_FILE)
    except OSError:
        pass


def report(problems, mode, host="rpi", dry_run=False):
    """Notify only on a change of health, or every REPEAT_HOURS while broken."""
    st = _load_state()
    now = time.time()
    healthy = not problems
    signature = "|".join(sorted(problems))

    if healthy:
        if not st.get("healthy", True):
            send(f"✅ *Book recovered* — `{host}` ({mode})\nAll 8 sleeves healthy again.",
                 dry_run=dry_run)
        _save_state({"healthy": True, "last_alert": 0, "signature": ""})
        return

    changed = st.get("healthy", True) or signature != st.get("signature", "")
    stale = (now - st.get("last_alert", 0)) > REPEAT_HOURS * 3600
    if changed or stale:
        head = "⚠️ *Book unhealthy*" if changed else "⚠️ *Still unhealthy*"
        body = "\n".join(f"• `{p}`" for p in problems[:10])
        if len(problems) > 10:
            body += f"\n• …and {len(problems) - 10} more"
        send(f"{head} — `{host}` ({mode})\n{len(problems)} problem(s):\n{body}", dry_run=dry_run)
        st = {"healthy": False, "last_alert": now, "signature": signature}
    else:
        st["healthy"] = False
        st["signature"] = signature
    _save_state(st)


if __name__ == "__main__":
    import sys
    if "--test" in sys.argv:
        ok = send("🤖 *cli_trader* test message — notifications are working.")
        print("delivered" if ok else "NOT delivered")
        sys.exit(0 if ok else 1)
    print("configured:", configured())
