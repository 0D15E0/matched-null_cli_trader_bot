#!/usr/bin/env python3
"""One-time Telegram setup: verify the bot and capture the chat id.

Run after creating the bot with @BotFather and sending it one message:

    python3 scripts/telegram_setup.py            # show bot + pending chats
    python3 scripts/telegram_setup.py --write    # also store TELEGRAM_CHAT_ID in .env

Reads TELEGRAM_BOT_TOKEN from .env. Never prints the token.
"""
import json, os, re, sys, urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV = os.path.join(REPO, ".env")


def env_get(key):
    try:
        m = re.search(rf'^\s*(?:export\s+)?{key}\s*=\s*["\']?([^"\'\n#]+)', open(ENV).read(), re.M)
        return m.group(1).strip() if m else None
    except OSError:
        return None


def env_set(key, value):
    """Rewrite .env with key=value, preserving everything else and 0600."""
    lines = []
    try:
        lines = [l for l in open(ENV).read().splitlines() if not l.startswith(key + "=")]
    except OSError:
        pass
    lines.append(f"{key}={value}")
    tmp = ENV + ".tmp"
    with open(tmp, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    os.chmod(tmp, 0o600)
    os.replace(tmp, ENV)


def api(token, method):
    """method may include a query string, e.g. getUpdates?offset=-1."""
    url = f"https://api.telegram.org/bot{token}/{method}"
    with urllib.request.urlopen(url, timeout=15) as r:
        return json.load(r)


def main():
    token = env_get("TELEGRAM_BOT_TOKEN")
    if not token:
        print("TELEGRAM_BOT_TOKEN not found in .env"); return 1
    try:
        me = api(token, "getMe").get("result", {})
    except Exception as e:
        print(f"getMe failed ({type(e).__name__}) - token wrong or no network"); return 1
    print(f"  bot: @{me.get('username')} (id {me.get('id')})")

    # offset=-1 asks for the MOST RECENT update regardless of confirmation
    # state. A plain getUpdates returns only *unconfirmed* updates, and any
    # earlier call (including a previous run of this script) confirms them --
    # so the obvious version reports "no messages yet" for a chat that has
    # clearly sent several. That cost a confused half hour on 2026-08-26.
    chats = {}
    for u in api(token, "getUpdates?offset=-1").get("result", []):
        msg = u.get("message") or u.get("edited_message") or {}
        ch = msg.get("chat") or {}
        if ch.get("id"):
            who = " ".join(str(x) for x in [ch.get("type"), ch.get("first_name"), ch.get("username")] if x)
            chats[ch["id"]] = who
    if not chats:
        print("  no messages yet - open the bot in Telegram and send it any message, then re-run")
        return 2
    for cid, who in chats.items():
        print(f"  chat_id {cid}  <- {who}")
    if "--write" in sys.argv:
        if len(chats) > 1:
            print("  more than one chat found; not guessing. Re-run with --chat-id <id>")
            return 3
        cid = list(chats)[0]
        env_set("TELEGRAM_CHAT_ID", str(cid))
        print(f"  wrote TELEGRAM_CHAT_ID={cid} to .env (0600)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
