#!/usr/bin/env python3
"""Health check for the cli_trader book on the Raspberry Pi.

Exit 0 = healthy (and silent, deliberately: this runs every 10 minutes from a
systemd timer, and a check that announces success trains you to ignore it).
Exit 1 = needs a human, with details printed and appended to the log.

Differences from the Mac-era check (check_live.py, since deleted): units are checked through systemd
rather than by pattern-matching process lists, and the state directory is read
from bot.env so the same script works in paper and live mode without editing.
"""
import glob, json, os, subprocess, sys, time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENV = os.path.join(REPO, "deploy", "pi", "bot.env")
SYMBOLS = ["BTC_USDT", "ETH_USDT", "XRP_USDT", "LTC_USDT",
           "DOGE_USDT", "TRX_USDT", "ADA_USDT", "SOL_USDT"]
STALE_SECONDS = 900          # poll interval is 120s; 7+ missed polls is real


def read_env():
    cfg = {}
    try:
        for line in open(ENV):
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                cfg[k.strip()] = v.strip()
    except OSError:
        pass
    return cfg


def main():
    cfg = read_env()
    state_dir = os.path.join(REPO, cfg.get("STATE_DIR", "state_paper"))
    mode = cfg.get("MODE", "?")
    problems = []

    # 1. systemd units. `is-active` is the supervisor's own opinion, which is
    #    more trustworthy than counting processes.
    for sym in SYMBOLS:
        unit = f"cli-trader@{sym}.service"
        r = subprocess.run(["systemctl", "is-active", unit],
                           capture_output=True, text=True)
        st = r.stdout.strip()
        if st != "active":
            problems.append(f"{sym}: unit {st}")

    # 2. Clock. A Pi has no RTC and HMAC request signing fails outright if the
    #    clock has drifted - it is a silent killer of live trading.
    r = subprocess.run(["timedatectl", "show", "-p", "NTPSynchronized", "--value"],
                       capture_output=True, text=True)
    if r.stdout.strip() != "yes":
        problems.append("clock NOT NTP-synchronised (HMAC auth will fail in live mode)")

    # 3. Per-sleeve state: staleness, halts, errors.
    states = sorted(glob.glob(os.path.join(state_dir, "*.state.json")))
    if len(states) < len(SYMBOLS):
        problems.append(f"only {len(states)}/{len(SYMBOLS)} state files in {state_dir}")
    now = time.time()
    for f in states:
        sym = os.path.basename(f).replace(".state.json", "")
        try:
            s = json.load(open(f))
        except Exception as e:
            problems.append(f"{sym}: unreadable state ({e})")
            continue
        age = now - s.get("last_poll_at", 0)
        if age > STALE_SECONDS:
            problems.append(f"{sym}: last poll {age/60:.0f} min ago (stale)")
        if str(s.get("last_action", "")).startswith("halted"):
            problems.append(f"{sym}: HALTED - {s.get('last_error','')[:120]}")
        elif s.get("last_error"):
            problems.append(f"{sym}: error - {s['last_error'][:120]}")

    # 4. Disk. An SD card that fills stops state writes, and this book's whole
    #    safety model rests on state being written every cycle.
    try:
        st = os.statvfs(REPO)
        free_mb = st.f_bavail * st.f_frsize / 1e6
        if free_mb < 200:
            problems.append(f"only {free_mb:.0f} MB free on / (state writes at risk)")
    except OSError:
        pass

    # Push alerts. Import lazily and defensively: the health check must keep
    # working (and keep its exit code meaningful) even if the notifier is
    # missing or broken.
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import notify
        notify.report(problems, mode, host=os.uname().nodename)
    except Exception as e:
        print(f"[notify] unavailable: {type(e).__name__}")

    if not problems:
        return 0

    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    report = "\n".join([f"[{stamp}] BOOK UNHEALTHY (mode={mode}, {len(problems)} problem(s))"]
                       + [f"  - {p}" for p in problems])
    print(report)
    try:
        os.makedirs(os.path.join(REPO, "logs_pi"), exist_ok=True)
        with open(os.path.join(REPO, "logs_pi", "healthcheck.log"), "a") as fh:
            fh.write(report + "\n")
    except OSError:
        pass
    return 1


if __name__ == "__main__":
    sys.exit(main())
