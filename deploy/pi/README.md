# The Raspberry Pi deployment

The book's permanent home. A laptop was always the wrong host: it sleeps
(see RUNBOOK.md §7 — the 50-minute freeze on 2026-08-25), it has a lid, and it
has a battery. This Pi has none of those.

## The host

    Raspberry Pi 3, Debian 13 (trixie), aarch64, kernel 6.18.34
    4 cores, 905 MB RAM (+904 MB zram swap), 6.6 GB SD
    Repo at ~/trader/cli_trader

The unit files ship with a `__TRADER_USER__` placeholder for the account that
owns the repo and runs the sleeves. Substitute it at install time:

```sh
sudo sh -c 'for u in deploy/pi/cli-trader*.service deploy/pi/cli-trader*.timer; do
      sed "s|__TRADER_USER__|'"$USER"'|g" "$u" > /etc/systemd/system/"$(basename "$u")"
    done'
sudo systemctl daemon-reload
```

This assumes the repo lives at `/home/<user>/trader/cli_trader`; adjust the
paths in the units if yours differs.

Runtime footprint measured with all 8 sleeves running: **206 MB of 905 MB**,
load average under 0.5. Building is the only heavy moment (~4 min at `-j2`).

## What runs

One `systemd` template unit per sleeve — `cli-trader@SYMBOL.service` — plus a
health-check timer. All eight read their configuration from a single file so
they can never disagree about mode or size:

    deploy/pi/bot.env        MODE, STATE_DIR, EQUITY, QUOTE_CAP, VOL_TARGET, ...

Restart semantics deliberately mirror the Mac's launchd config: `Restart=on-failure`
restarts a **crash**, but a clean `systemctl stop` stays stopped, so stopping a
sleeve actually stops it instead of fighting the supervisor.

## Verified on deployment day (2026-08-25)

- **Numerically identical to the Mac.** On deployment day, `backtest --strategy
  ensemble_vote --vol-target 0.20 --start 2024-01-01` on BTC returned the same
  total return (62.40%), Sharpe (1.02), drawdown (23.79%) and trade count (61)
  on both machines. Re-verified 2026-08-26 for the configuration now live
  (`--sparams enterVotes=2,exitVotes=0 --vol-target 0.30 --vol-window 90`,
  dev window): Sharpe 1.32, drawdown 39.34%, 77 trades on both. Without this,
  nothing measured on the Mac would apply here.
- **Survives reboot unattended.** Full reboot: back in ~25 s, 8/8 sleeves
  auto-started, NTP synced within 5 s, and every open position restored with
  its entry price intact.
- **Clock ordering hardened.** `systemd-time-wait-sync` is enabled so
  `time-sync.target` is only reached once the clock is *actually* synchronised.
  A Pi has no RTC, and HMAC request signing fails outright on a wrong clock —
  starting a live sleeve before sync is a guaranteed authentication failure.

## Operating it

```sh
ssh youruser@your-pi.lan
cd ~/trader/cli_trader

python3 scripts/check_pi.py              # health; silent when fine, exit 1 when not
systemctl is-active cli-trader@BTC_USDT  # one sleeve
journalctl -u cli-trader@BTC_USDT -n 50  # one sleeve's history
tail -20 logs_pi/BTC_USDT.log            # its output

sudo systemctl restart cli-trader@BTC_USDT     # one sleeve
sudo systemctl stop 'cli-trader@*'             # the whole book (stays stopped)
```

The health check runs every 10 minutes from `cli-trader-healthcheck.timer` and
checks unit state, per-sleeve staleness, halts, errors, **NTP sync**, and free
disk. It writes to `logs_pi/healthcheck.log` only when something is wrong.

## Going live — the migration, in order

The Pi currently runs **`MODE=paper`** (`state_paper/`), which touches no keys
and no real balances.

> **Never run two live books against one Poloniex account.** Each would see
> base currency the other bought, and both would halt on
> `halted-balance-mismatch` — or worse, double-trade. The Mac's launchd book is
> still the live one.

1. **Let the paper book run** until `parity` shows a signal match near 100% on
   several sleeves. At ~2 signals per sleeve per month, that is weeks, not days.
   ```sh
   ./build/cli_trader parity --symbol BTC_USDT --period 14400 \
       --strategy ensemble_vote --state-dir state_paper --data-dir data
   ```
2. **Copy the credentials** to the Pi — `.env` with `API-KEY` / `SECRET-KEY`,
   `chmod 600`. It is deliberately NOT transferred by the deployment tarball.

   The account key is IP-restricted, but **no change is needed**: the Mac and
   the Pi sit behind the same router and NAT out through the same public
   address (verified 2026-08-25 — both report the same egress IP), so Poloniex
   sees one address either way. The real exposure is different: the ISP
   assigning a new public IP would break BOTH hosts at once, and it would
   surface as authentication failures in `last_error` rather than as anything
   obviously network-shaped.
3. **Stop the Mac book first**, and confirm it is stopped:
   ```sh
   # on the Mac
   ./scripts/uninstall_launchd.sh && pgrep -f 'cli_trader run' | wc -l   # expect 0
   ```
4. **Switch the Pi to live** — edit `deploy/pi/bot.env`: `MODE=live`,
   `STATE_DIR=state_live`, then
   ```sh
   sudo systemctl restart 'cli-trader@*'
   ```
5. **Watch the first cycle closely.** A fresh live book cold-starts into
   whatever positions the strategy already holds, at today's prices — the same
   cold-start penalty documented in RUNBOOK.md §4.

## Known gaps

- **No log rotation** on `logs_pi/` yet. ~720 lines/day/sleeve on a 6.6 GB card
  is slow-burning, but it is unbounded.
- **SD cards die.** This one is new; the previous card failed after six months
  idle and is what started this whole exercise. `check_pi.py` watches free
  space but cannot see wear. Keep the deployment reproducible rather than
  precious — everything here is a tarball and four unit files.
- **The login password** is whatever was set at imaging. Change it from the
  default, and disable SSH password auth entirely once key auth works. Never
  embed it in a command or a runbook — for unattended `systemctl` calls use the
  scoped `NOPASSWD` sudoers rule shown in RUNBOOK.md's Conventions section.
