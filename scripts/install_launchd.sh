#!/bin/zsh
# DECOMMISSIONED 2026-08-25. The book moved to the Raspberry Pi (deploy/pi/)
# and the Mac-era health check (scripts/check_live.py) was deleted - the
# healthcheck job this installs would run a script that no longer exists.
# Kept only as a reference for the launchd setup. Do not run as-is.
echo "DECOMMISSIONED: the book runs on the Pi now (see deploy/pi/README.md)." >&2
echo "check_live.py was deleted; this installer would create a broken job." >&2
exit 1
# Installs launchd jobs for the live trading book: one KeepAlive job per
# sleeve plus a 10-minute health check. Idempotent - reinstalls over itself,
# which is also how sleeves are restarted onto a new binary or new settings.
#
# Restart semantics, chosen deliberately:
#   KeepAlive = { SuccessfulExit = false }
# restarts a sleeve that CRASHES but leaves one alone that exited cleanly -
# `pkill -INT` produces a clean exit, so stopping still means stopping.
# After a reboot the book starts at LOGIN (user LaunchAgents; a headless
# reboot with no login does not start them).
#
# bootout and bootstrap must not race: launchd returns EIO if a label is
# bootstrapped while its previous instance is tearing down. Order therefore
# is: write all plists -> bootout all -> wait for processes to exit ->
# bootstrap all, with retries.
set -e
set -u
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$REPO/build/cli_trader"
AGENTS="$HOME/Library/LaunchAgents"
UID_N="$(id -u)"
# Each sleeve's share of the book, in USDT. Sizing budgets against
# min(venue cash, this cap); a raised cap takes effect at each sleeve's NEXT
# entry (positions are sized at entry and never resized mid-trade).
SLEEVE_CAP="${SLEEVE_CAP:-487}"
SYMBOLS=(BTC_USDT ETH_USDT XRP_USDT LTC_USDT DOGE_USDT TRX_USDT ADA_USDT SOL_USDT)

mkdir -p "$AGENTS" "$REPO/logs_live"

# --- 1. write every plist --------------------------------------------------
for sym in "${SYMBOLS[@]}"; do
  plist="$AGENTS/com.cli-trader.live.$sym.plist"
  cat > "$plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.cli-trader.live.$sym</string>
  <key>WorkingDirectory</key><string>$REPO</string>
  <key>ProgramArguments</key><array>
    <!-- caffeinate -i holds an idle-sleep assertion for the lifetime of the
         bot. Without it macOS idle-sleeps (this Mac is set to sleep after 1
         minute on battery) and FREEZES every sleeve: on 2026-08-25 the book
         sat suspended 11:56-12:47 local. Nothing was lost that time because
         no 4h bar closed in the window, but a sleep straddling a bar close
         means the order fires late, at a different price than the strategy
         decided on. NOTE: -i does not survive closing the lid; only the Pi
         (or a machine that never sleeps) fixes that properly. -->
    <string>/usr/bin/caffeinate</string><string>-i</string>
    <string>$BIN</string><string>run</string>
    <string>--symbol</string><string>$sym</string>
    <string>--period</string><string>14400</string>
    <string>--strategy</string><string>ensemble_vote</string>
    <string>--vol-target</string><string>0.20</string>
    <string>--mode</string><string>live</string>
    <string>--equity</string><string>$SLEEVE_CAP</string>
    <string>--quote-cap</string><string>$SLEEVE_CAP</string>
    <string>--poll-interval</string><string>120</string>
    <string>--data-dir</string><string>data</string>
    <string>--state-dir</string><string>state_live</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><dict><key>SuccessfulExit</key><false/></dict>
  <key>ThrottleInterval</key><integer>60</integer>
  <key>ProcessType</key><string>Background</string>
  <key>StandardOutPath</key><string>$REPO/logs_live/$sym.log</string>
  <key>StandardErrorPath</key><string>$REPO/logs_live/$sym.log</string>
</dict></plist>
PLIST
  plutil -lint -s "$plist"
done

# --- 2. boot out old instances, wait for them to die ------------------------
for sym in "${SYMBOLS[@]}"; do
  launchctl bootout "gui/$UID_N/com.cli-trader.live.$sym" 2>/dev/null || true
done
for i in $(seq 1 30); do
  pgrep -f 'state_live$' >/dev/null 2>&1 || break
  sleep 1
done

# --- 3. bootstrap everything, with retries for launchd tombstones -----------
for sym in "${SYMBOLS[@]}"; do
  ok=0
  for attempt in 1 2 3 4 5; do
    if launchctl bootstrap "gui/$UID_N" "$AGENTS/com.cli-trader.live.$sym.plist" 2>/dev/null; then
      ok=1; break
    fi
    sleep 3
  done
  if [ "$ok" != 1 ]; then
    echo "FAILED to bootstrap $sym" >&2
    exit 1
  fi
done

# --- 4. health check: every 10 minutes, macOS notification on failure -------
HC="$AGENTS/com.cli-trader.healthcheck.plist"
cat > "$HC" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>com.cli-trader.healthcheck</string>
  <key>WorkingDirectory</key><string>$REPO</string>
  <key>ProgramArguments</key><array>
    <string>/usr/bin/python3</string>
    <string>$REPO/scripts/check_live.py</string>
    <string>--expect</string><string>8</string>
    <string>--notify</string>
  </array>
  <key>StartInterval</key><integer>600</integer>
  <key>RunAtLoad</key><true/>
  <key>StandardOutPath</key><string>$REPO/logs_live/healthcheck.log</string>
  <key>StandardErrorPath</key><string>$REPO/logs_live/healthcheck.log</string>
</dict></plist>
PLIST
plutil -lint -s "$HC"
launchctl bootout "gui/$UID_N/com.cli-trader.healthcheck" 2>/dev/null || true
launchctl bootstrap "gui/$UID_N" "$HC" 2>/dev/null || {
  sleep 3
  launchctl bootstrap "gui/$UID_N" "$HC"
}

echo "installed: ${#SYMBOLS[@]} sleeve jobs (cap \$$SLEEVE_CAP each) + healthcheck (gui/$UID_N)"
