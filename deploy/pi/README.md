# Dedicated Linux deployment

The book is intended to run on a dedicated always-on Linux host. A laptop is
not a suitable production host because it can sleep, suspend, or lose network
connectivity.

## The host

    Linux host, supported Debian-family release, aarch64 or compatible target
    Repo at ~/trader/cli_trader (adjust to the local deployment path)

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

Measure runtime memory and build time on the target host before enabling all
sleeves. Building is the heavy operation; do not compile during production
operation.

## What runs

One `systemd` template unit per sleeve — `cli-trader@SYMBOL.service` — plus a
health-check timer. All eight read their configuration from a single file so
they can never disagree about mode or size:

    deploy/pi/bot.env        MODE, STATE_DIR, EQUITY, QUOTE_CAP, VOL_TARGET, ...

Alongside them, `cli-trader-telegram.service` runs the phone-side bot. It does
two jobs: it answers the read-only queries (`/report`, `/status`, `/health`,
`/balances`, `/log`), and between polls it pushes a message for every fill the
book makes, by tailing `state_live/<SYM>.trades.jsonl`. That is deliberately
outside the trading processes — a sleeve must never block on Telegram — and it
is why the health check now also asserts this unit is alive. See RUNBOOK §3.

Restart semantics use `Restart=on-failure`: a crash restarts, but a clean
`systemctl stop` stays stopped.

## Deployment validation checklist

- **Replay parity.** Run the same bounded backtest on the development machine
  and target host, then compare trades, metrics, binary revision, and data
  checksums. Do not use contaminated forward data as validation.
- **Restart recovery.** Reboot or restart the paper deployment and confirm all
  sleeves recover state, synchronize time, and restore entry information.
- **Clock ordering.** Ensure `time-sync.target` is reached only after the host
  clock is synchronized; signed exchange requests require correct time.

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

New deployments should start in **`MODE=paper`** (`state_paper/`), which touches
no keys and no real balances.

> **Never run two live books against one exchange account.** Each would see
> base currency the other bought, and both would halt on
> `halted-balance-mismatch` — or worse, double-trade. Confirm that any other
> deployment using the account is stopped before switching this host to live.

1. **Let the paper book run** until `parity` shows a signal match near 100% on
   several sleeves. At ~2 signals per sleeve per month, that is weeks, not days.
   ```sh
   ./build/cli_trader parity --symbol BTC_USDT --period 14400 \
       --strategy ensemble_vote --state-dir state_paper --data-dir data
   ```
2. **Install credentials out of band** on the target host — `.env` with the
  required API variables, mode `0600`. Credentials are deliberately NOT
  transferred by the deployment tarball.
3. **Confirm any other book using the account is stopped** before switching
  this host to live.
   ```sh
  pgrep -f 'cli_trader run' || true
   ```
4. **Switch the target host to live** — edit the local-only
  `deploy/pi/bot.env` created from `bot.env.example`: `MODE=live`,
   `STATE_DIR=state_live`, then
   ```sh
   sudo systemctl restart 'cli-trader@*'
   ```
5. **Watch the first cycle closely.** A fresh live book cold-starts into
   whatever positions the strategy already holds, at today's prices — the same
   cold-start penalty documented in RUNBOOK.md §4.

## Known gaps

- **No log rotation** on `logs_pi/` yet. Log growth is slow-burning, but it is
  unbounded on constrained storage.
- **Host storage can fail.** `check_pi.py` watches free space but cannot see
  wear. Keep the deployment reproducible and maintain backups of operational
  state.
- **The login password** is whatever was set at imaging. Change it from the
  default, and disable SSH password auth entirely once key auth works. Never
  embed it in a command or a runbook — for unattended `systemctl` calls use the
  scoped `NOPASSWD` sudoers rule shown in RUNBOOK.md's Conventions section.
