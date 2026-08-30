# Runbook: operating the live trading book

The book runs on a Raspberry Pi. Every command below is copy-pasteable and
runs over SSH from any machine on the LAN.

**Conventions.** Commands address the Pi as `"$PI"`. Export it once per shell:

```sh
export PI=youruser@your-pi.lan     # or youruser@192.168.x.y
```

`sudo` commands assume you can elevate on the Pi non-interactively. Either run
them from an interactive SSH session, or grant the trading user a scoped
`NOPASSWD` rule for the units only — never put a password in a command:

```sh
# /etc/sudoers.d/cli-trader  (visudo -f, mode 0440)
youruser ALL=(root) NOPASSWD: /usr/bin/systemctl * cli-trader@*, \
                              /usr/bin/systemctl * cli-trader-*
```

> **Status: LIVE on the Pi since 2026-08-25, ~21:04 CEST.** Real money, real
> orders, against the Poloniex account in `.env`. The Mac's launchd book was
> stopped and **uninstalled** the same day — its plists are deleted, so it
> cannot restart on reboot. There is exactly one book.

    host      "$PI"                     (see Conventions above)
    repo      ~/trader/cli_trader       on the Pi
    strategy  ensemble_vote enter>=2 / exit<=0 (--sparams enterVotes=2,exitVotes=0),
              8 sleeves, --vol-target 0.30 --vol-window 90
    config    deploy/pi/bot.env         (one file governs all eight sleeves)
    state     state_live/               logs: logs_pi/

Deployment details, hardware, and how it was validated:
[deploy/pi/README.md](../deploy/pi/README.md). How the software itself is
structured: [ARCHITECTURE.md](ARCHITECTURE.md).

---

## 1. The strategy in one page

Eight independent sleeves — BTC, ETH, XRP, LTC, DOGE, TRX, ADA, SOL against
USDT — on 4h candles, each holding an equal share of the book.

`ensemble_vote` is a majority vote of three trend rules at published defaults:
**long while at least 2 of 3 want to be long; sell only once all three have
quit** (`enterVotes=2, exitVotes=0`, live since 2026-08-26 — until then the
book sold when the count fell to 1; PROFITABILITY_PLAN.md addendum 15 has the
sweep behind the change and its known cost, deeper per-sleeve pullbacks).
Exits are never blocked.

| member | wants to be long while |
|---|---|
| `tsmom` | 90-bar trailing return > +5% (entry also gated by annualized vol <= 60%) |
| `faber_ma` | price above its own 200-bar moving average |
| `donchian` | after a breakout of the prior 55-bar high, until a 20-bar low or ATR trailing stop |

| | |
|---|---|
| Sizing | `30% / (annualized volatility at entry, 90-bar window)`, capped at 100%, fixed for the life of the trade. Was 20%/30-bar until 2026-08-25, when the 90-bar window measured as a drawdown reducer over the noisier 30-bar default |
| Execution | decide at bar close, fill at the next bar's open |
| Real costs | 0.125% taker fee (account's actual tier) + spread |
| Cadence | ~2 signals per sleeve per month |

**Do not run this on a lower timeframe.** Measured on clean data: at 15m the
same rule keeps a gross Sharpe of 1.24 but turns in -89% net, because turnover
goes from 197 trades to 4,307. Breakeven needs ~0.05%/side against the 0.125%
actually paid.

---

## 2. Daily — the one command

```sh
ssh "$PI" 'cd trader/cli_trader && python3 scripts/status_live.py && python3 scripts/check_pi.py && echo HEALTHY'
```

(`check_pi.py` is silent when all is well — the trailing `echo` is your explicit
green light. It exits 1 with details when something is wrong. The same script
runs unattended every 10 minutes from the systemd timer; it is the only health check —
the Mac-era `check_live.py` was deleted when the book moved here.)

```
sleeve pos   last action    value$    entry      now   uP&L$  uP&L%  rP&L$  W-L  signal  flags
BTC    LONG  hold           213.54   79,240   79,480   +0.11  +0.3%  +0.00  0-0     3h
ADA    flat  hold             0.00        -   0.2156   +0.00  +0.0%  +0.00  0-0     3h
```

- **pos** — `LONG` (holding the coin) or `flat` (all cash, zero exposure)
- **last action** — `hold` = no change this cycle (the normal state);
  `buy`/`sell` = an order fired; `scanning` = flat and watching;
  `halted-*` = trading suspended, see §5
- **signal** — time since the strategy last changed its mind
- **flags** — empty is good. `HALTED`, `ERR`, `STALE` are the three worth acting on

Add `--trades` for each sleeve's last real fill.

Most days this is eight rows of `hold` and pennies of movement. That is the
strategy working, not a fault.

---

## 3. Monitoring and health

```sh
# health check: silent when fine, exit 1 + details when not
ssh "$PI" 'cd trader/cli_trader && python3 scripts/check_pi.py'

# real account holdings, straight from Poloniex (read-only)
ssh "$PI" 'cd trader/cli_trader && ./build/cli_trader balances'

# all eight units at a glance
ssh "$PI" 'systemctl is-active cli-trader@{BTC,ETH,XRP,LTC,DOGE,TRX,ADA,SOL}_USDT.service'

# one sleeve, full detail
ssh "$PI" 'cd trader/cli_trader && ./build/cli_trader status --symbol BTC_USDT --state-dir state_live --trades 10'

# is the health timer alive, and when does it next fire?
ssh "$PI" 'systemctl list-timers cli-trader-healthcheck --no-pager'

# anything the health check has complained about
ssh "$PI" 'cd trader/cli_trader && tail -30 logs_pi/healthcheck.log'
```

The check runs every 10 minutes and watches unit state, per-sleeve staleness,
halts, errors, **NTP sync**, and free disk. It only writes when something is
wrong. It also checks `cli-trader-telegram.service`, because that unit is what
pushes the alerts below — a book that trades in silence looks healthy from the
phone, and this is the only thing that would notice.

### Telegram: what arrives without being asked

Two pushes, from two different watchers, over the same bot:

| when | sender | what |
|---|---|---|
| the book **breaks** or recovers | `scripts/notify.py`, from the 10-minute health check | one message on the transition, a reminder every 6 h while broken |
| the book **buys or sells** | `scripts/trade_alerts.py`, from the bot's poll loop | one message per fill: size, price, cost, and on an exit the round trip's P&L |

The fill alerter tails `state_live/<SYM>.trades.jsonl` — the venue-confirmed
record the sleeves already append to — rather than being wired into the trading
loop, so it cannot delay, block or crash a sleeve. Worst-case delay between a
fill and the message is one long-poll timeout, ~30 s.

```sh
# is it configured, and how far has it read?
ssh "$PI" 'cd trader/cli_trader && python3 scripts/trade_alerts.py --status'

# prove the whole path without waiting for a trade: re-sends the newest
# real fill, marked "test"
ssh "$PI" 'cd trader/cli_trader && python3 scripts/trade_alerts.py --test'

# what the alerter has been doing
ssh "$PI" 'cd trader/cli_trader && tail -20 logs_pi/telegram.log'
```

It announces nothing on its first run: an unknown sleeve is adopted at its
trade log's current length, so installing it (or losing `logs_pi/`) never
replays history into the chat. `--reset` re-adopts deliberately.

### The number that actually matters

```sh
ssh "$PI" 'cd trader/cli_trader && ./build/cli_trader parity \
    --symbol BTC_USDT --period 14400 --strategy ensemble_vote \
    --state-dir state_live --data-dir data'
```

**Signal match rate, not P&L.** It replays stored candles through the backtest
engine and matches the fills it would have made against the fills actually
logged. Near 100% means the thing running is the thing that was measured.
Anything else means it is not, and no backtest result applies to it.

P&L over any short window means nothing here: the Sharpe error bar is +/-0.62
over 2.6 *years*.

---

## 4. Maintenance

```sh
# restart one sleeve
ssh "$PI" 'sudo systemctl restart cli-trader@BTC_USDT'

# restart the whole book
ssh "$PI" 'sudo systemctl restart "cli-trader@*"'

# STOP the book (stays stopped; on-failure restart does not fight a clean stop)
ssh "$PI" 'sudo systemctl stop "cli-trader@*"'

# start it again
ssh "$PI" 'for s in BTC ETH XRP LTC DOGE TRX ADA SOL; do sudo systemctl start cli-trader@${s}_USDT; done'

# stop it starting at boot (leaves it running now)
ssh "$PI" 'sudo systemctl disable "cli-trader@*"'
```

### Changing size — the periodic re-cap

`--quote-cap` is **static**: a sleeve sizes against `min(account USDT, cap)`.
Profits accumulate as idle cash rather than compounding, and losses do not
de-lever. The backtest compounds in both directions, so live sizing drifts from
the measured strategy as P&L accumulates — negligible at +/-10%, material by
+/-30%.

Re-cap quarterly, or when the book drifts ~20% from 8 x cap:

```sh
ssh "$PI" 'cd trader/cli_trader && ./build/cli_trader balances'   # get the total
ssh "$PI" 'cd trader/cli_trader && sed -i "s/^EQUITY=.*/EQUITY=NNN/; s/^QUOTE_CAP=.*/QUOTE_CAP=NNN/" deploy/pi/bot.env && sudo systemctl restart "cli-trader@*"'
```

New caps apply at each sleeve's **next entry** — positions are sized at entry
and never resized mid-trade. Re-capping *down* after losses is what restores
the measured risk profile, and it is the direction people skip.

### Keeping data fresh

`run` maintains the stores itself. To force a catch-up after downtime:

```sh
ssh "$PI" 'cd trader/cli_trader && for s in BTC ETH XRP LTC DOGE TRX ADA SOL; do ./build/cli_trader fetch --symbol ${s}_USDT --period 14400 --data-dir data; done'
```

### Log rotation (not yet automated)

~720 lines/day/sleeve, unbounded, on a 6.6 GB card:

```sh
ssh "$PI" 'cd trader/cli_trader && for f in logs_pi/*.log; do tail -5000 "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done; df -h / | tail -1'
```

---

## 5. Debugging

```sh
# why did a sleeve fail or restart?
ssh "$PI" 'journalctl -u cli-trader@BTC_USDT -n 60 --no-pager'

# follow one sleeve live
ssh "$PI" 'tail -f trader/cli_trader/logs_pi/BTC_USDT.log'

# every error across the book
ssh "$PI" 'cd trader/cli_trader && grep -ihE "error|fail|halt" logs_pi/*.log | tail -30'

# any unit in a failed state
ssh "$PI" 'systemctl --failed --no-pager'

# host health: memory, disk, load, clock
ssh "$PI" 'free -m | head -2; df -h / | tail -1; uptime; timedatectl | grep -E "synchronized|NTP"'
```

### After ANY change to `bot.env`: verify the mode from the running processes

```sh
ssh "$PI" 'ps -eo args= | grep "[c]li_trader run" | grep -o "\-\-mode [a-z]*" | sort | uniq -c'
```

Expect `8 --mode live`. **Incident 2026-08-25:** a scripted edit to `bot.env`
matched a *comment* containing `MODE=paper` instead of the real assignment,
the file was pushed, and all eight sleeves restarted in paper mode against
`state_live/` for 62 seconds — real positions unattended, and one paper fill
away from corrupting the live records. No bar closed in the window, so nothing
happened. Two consequences: the unit now has an `ExecStartPre` guard that
refuses to start paper against `state_live` (or live against anything else),
and edits to `bot.env` must anchor on the line start (`sed -E 's/^MODE=.*/…/'`),
never on the bare token.

### `halted-balance-mismatch`

The bot found base currency it did not buy and **stopped trading rather than
guessing** — it refuses to sell coins that might be yours or another bot's.
This is correct behaviour. It keeps re-checking and resumes by itself once the
account agrees.

Diagnose by comparing what the bot thinks it holds against the venue:

```sh
ssh "$PI" 'cd trader/cli_trader && ./build/cli_trader balances'
ssh "$PI" 'cd trader/cli_trader && python3 scripts/status_live.py'
```

Only if you deliberately want the bot to take over whatever the account holds,
add `--adopt-venue-position` to the unit. **Never** do this to silence a halt
you have not explained.

### The API secret was in the systemd journal (fixed 2026-08-26)

The unit used to list `.env` as an `EnvironmentFile`. systemd rejects names
with dashes (`API-KEY`, `SECRET-KEY`) — and writes the rejected line, secret
included, to the journal on every sleeve start. The line was removed (the
binary parses `.env` itself) and the journal was rotated and vacuumed. Check
it stays clean:

```sh
ssh "$PI" 'sudo journalctl --no-pager 2>/dev/null | grep -c SECRET-KEY'   # expect 0
```

The secret sat in a root-readable file on a home-LAN Pi for ~15 hours. Low
exposure, but **rotating the Poloniex API key is the conservative call** — it
is your decision; the new key goes into `.env` and the sleeves restart.

### Authentication failures

Two causes, and they look identical in `last_error`:

1. **Clock drift.** A Pi has no RTC and HMAC signing fails outright on a wrong
   clock. `systemd-time-wait-sync` is enabled so units wait for real sync, but
   check: `timedatectl | grep synchronized`.
2. **The API key is IP-restricted** to this network's public address. The Mac
   and Pi share it (same NAT), so moving hosts changed nothing — but if the ISP
   assigns a new public IP, **both** break at once, and the symptom points at
   auth rather than at networking. Check with
   `ssh "$PI" 'curl -s https://api.ipify.org'`.

### If SSH from a Mac terminal fails with "No route to host"

Not a network fault. macOS 15+ requires **Local Network** permission per app.
Grant it in System Settings → Privacy & Security → Local Network, or run from
a terminal that already has it. Symptom: the LAN routes show `!` (reject) in
`netstat -rn -f inet`, and even the router is unreachable while the internet
works fine.

---

## 6. What to expect

Measured on the 2024-01 → 2026-08 window (descriptive — see §7):

| | portfolio | buy & hold basket |
|---|---|---|
| CAGR | 17.1% | 15.8% |
| Sharpe | 1.06 | 0.55 |
| Max drawdown | **21.1%** | 60.6% |

**You make roughly what the basket makes and lose less than half as much in
the bad stretch.** That is the entire trade. You will spend bull markets
watching the basket run away and be flat during some of the best weeks.

Read the two columns differently. The drawdown reduction is diversification and
position sizing doing arithmetic — mean sleeve Sharpe 0.58 predicted 0.86 and
delivered 1.06. That keeps working. Every excess-Sharpe figure sits inside its
own error bar and is **not** established.

Signs something is genuinely wrong:

- parity signal match not converging toward ~100%
- `last_error` populated for more than a cycle or two
- a sleeve trading far more often than ~weekly
- drawdown materially worse than the basket's — the one thing this is for

---

## 7. Provenance, and how to read the numbers

The 2024+ window was frozen as a single-look holdout and has since been read
many times (see `experiments/holdout.json`, which records the contamination).
Numbers above are **descriptive of that period**, not out-of-sample evidence.

The pre-registered test is `forward_tests` → `live-8sleeve-ensemble-exit0` in
that file (its predecessor, `paper-8sleeve-ensemble`, was voided when the exit
rule changed): data from 2026-08-26 onward, readable no earlier than
**2027-08-26**,
with **drawdown <= half the basket's** as the primary criterion and excess
Sharpe demoted to secondary and flagged underpowered in advance.

**The live book is that test's measurement vehicle.** Real fills make it
stricter than registered, not looser.

---

## 8. Still open

1. **The order path has never been independently verified.** `balances` and
   `/feeinfo` authenticate correctly against the real account, and orders have
   been placed and filled — but there is no test covering cancel or partial-fill
   handling.
2. **No automated tests.** `enable_testing()` with no `add_test` following it.
   What exists is `causality_check` (24 families, zero look-ahead violations),
   which is one property well covered, not a suite.
3. **Shutdown does not wait for an in-flight order.** The idempotent client
   order id is what stops that becoming a duplicate; the window still exists.
4. **No log rotation** (§4 has the manual command).
5. **Alerting is one bot, one chat, one token.** Health transitions and fills
   both push to Telegram (§3), but there is no second channel: if the Pi loses
   its network, or the token is revoked, nothing pages you about the fact that
   nothing is paging you. The health check knowing the telegram unit is dead
   only helps while the unit can still send.
6. **SD cards die.** The previous one failed after six months idle. Keep the
   deployment reproducible rather than precious — it is a tarball and four
   unit files.
