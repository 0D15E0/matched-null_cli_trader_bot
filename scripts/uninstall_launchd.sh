#!/bin/zsh
# Removes the launchd jobs AND stops the processes they supervise.
UID_N=$(id -u)
for sym in BTC_USDT ETH_USDT XRP_USDT LTC_USDT DOGE_USDT TRX_USDT ADA_USDT SOL_USDT; do
  launchctl bootout "gui/$UID_N/com.cli-trader.live.$sym" 2>/dev/null || true
  rm -f "$HOME/Library/LaunchAgents/com.cli-trader.live.$sym.plist"
done
launchctl bootout "gui/$UID_N/com.cli-trader.healthcheck" 2>/dev/null || true
rm -f "$HOME/Library/LaunchAgents/com.cli-trader.healthcheck.plist"
echo "launchd jobs removed; any supervised bots have been stopped"
