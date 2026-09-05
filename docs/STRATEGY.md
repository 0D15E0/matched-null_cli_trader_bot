# The strategy, in plain English

This is the document to read before the code. It explains what the live book
does, in ordinary language, and — more importantly — why it does *this* and
not one of the thousands of other things we tried. Operations live in
[RUNBOOK.md](RUNBOOK.md); the strategy-search evidence lives in
[TOURNAMENT.md](TOURNAMENT.md).
The research-only inverse-volatility portfolio variant is explained in
[ENSEMBLE_INVOL.md](ENSEMBLE_INVOL.md).

## What it does, in one paragraph

The book splits its money into **eight equal slices** — Bitcoin, Ethereum,
XRP, Litecoin, Dogecoin, Tron, Cardano, Solana, each traded against USDT.
Every four hours, each slice asks one question: *is this coin in an uptrend or
not?* If yes, it holds the coin. If no, it holds cash. That's the whole
strategy: **ride uptrends, stand aside the rest of the time.** It never bets
on prices falling, never uses leverage, and never puts a slice all-in — the
size of every position is scaled down by how violent the coin currently is.

## How each slice decides

"Is this an uptrend?" sounds simple, but there are dozens of ways to measure
it, and — this was one of our main findings — none of them is measurably
better than the others. So instead of picking one, we let **three classic
trend rules vote**, each looking at the trend through a different lens:

| rule | in plain terms | age of the idea |
|---|---|---|
| Momentum (`tsmom`) | "Is the price higher than it was 15 days ago, by more than 5%?" | academic standard since the 1980s–2012 |
| Moving average (`faber_ma`) | "Is the price above its own average of the last ~33 days?" | Faber 2007, and folk knowledge long before |
| Breakout (`donchian`) | "Did the price recently punch through its ~9-day high, and has it not broken down since?" | Donchian's rule, 1960s; the Turtle traders |

**The slice is long while at least 2 of the 3 say "uptrend", and goes to cash
only once all three have stopped saying it.** Exits are never blocked — any
slice can always get out.

(Until 2026-08-26 it went to cash as soon as the count fell to 1. Selling
later — waiting for the last member to give up — cut the number of trades by
two-thirds and scored higher on every development year, at the price of riding
pullbacks deeper before exiting; it did *not* pass our own pre-registered
adoption test and did worse on the most recent two years, so it was adopted as
an explicit judgement call, not as a proven improvement. PROFITABILITY_PLAN.md
addendum 15 has the full comparison; the live forward test that judges it runs
to 2027-08-26.)

Why a vote instead of the best rule? Because when we tested the rules head to
head, their ranking reshuffled every time we added a few days of data. Picking
"the best" would mean picking noise. When a choice is noise, you don't make it
— you spread across it, exactly like owning eight coins instead of one.

## How big each position is

When a slice enters, it does **not** go all-in. It sizes the position so the
slice runs at roughly 30% annualized volatility:

> position = 30% ÷ (how volatile the coin has been over the last ~15 days)

(It ran at 20% until 2026-08-25. Raising it was a deliberate risk-level choice: measured across 20–40%, return and worst-loss scale together while risk-adjusted return stays flat, so the target is a preference, not a tuning. The 15-day window replaced a 5-day one because the shorter estimate was noisy and sized entries badly after quiet spells.)

A calm coin gets a bigger position; a wild one gets a smaller one. This single
rule is, by our measurements, **the most valuable line of code in the repo** —
more valuable than any signal. It is why the book's worst historical loss is
around a fifth of its value while just holding the same coins lost two thirds.

The size is set once, at entry, and left alone until the trade closes.

## Why we chose this — the honest history

This strategy was not designed; it is what was left standing. The short
version of a long hunt:

**1. We tested essentially everything else first.** Twenty-four strategy
families — classic technical rules, academic papers (Avellaneda–Lee mean
reversion, realized-skewness effects, López de Prado's fractional
differentiation, Sornette's bubble-detection model, and more) — were tuned and
bred through evolutionary tournaments: **~53,000 candidate strategies** in
total. Coin-flip control strategies rode along in every tournament, selected
and mutated like everything else, so we could see what pure luck scores.

**2. The result was a clean negative.** On data none of the candidates had
seen, nothing reliably beat just holding the coins — and nothing meaningfully
beat the coin-flip controls. The tournaments could tell *hopeless* ideas from
*plausible* ones, but among the plausible ones, in-sample ranking predicted
nothing. If someone offers you a crypto trading signal, this is the base rate
to remember.

**3. Two things did survive, and both are arithmetic, not prophecy.**
- **Volatility-targeted sizing**: cutting position size when markets get
  violent removes risk you were never being paid for.
- **Diversification**: eight partially-uncorrelated slices are smoother than
  one, by a formula that has no opinion about the future. When we predicted
  the portfolio's quality from its parts, the prediction came out almost
  exactly right — twice. Forecasts kept being wrong; arithmetic kept working.

**4. The trend filter earns its place through the drawdown, not the profit.**
A slice that goes to cash in a crash is the mechanism behind "lose half as
much." The specific trend rule barely matters (finding #1 again) — hence the
vote of three rather than a champion.

**5. The timeframe is not a style choice.** We measured the same strategy on
15-minute bars: the *signal* still worked before costs, but it traded 22x more
often, and fees turned a good gross result into a total loss. At 4-hour bars,
trading costs eat about 10% of the edge; at 15 minutes they eat 350% of it.
Four hours is roughly where this idea stops paying rent to the exchange.

**6. Long-only is a live-engine limit, stated plainly.** The live software can
hold a coin or hold cash; it cannot bet on falls. "Flat" is this book's maximum
bearishness. (The *backtester* gained a long/short mode in Aug 2026 to measure
whether shorting via Poloniex perpetuals would help; the adversarially-verified
answer was "not on risk-adjusted evidence", so the live book stays
long/flat.)

## What to expect

Measured over 2024–2026 (see the caveat below):

| | this book | just holding the 8 coins |
|---|---|---|
| yearly growth | ~17% | ~16% |
| worst peak-to-trough loss | **~21%** | **~61%** |

**You make roughly what holding makes, and lose a third as much in the worst
stretch.** That is the entire product. The cost is psychological: in a raging
bull market the book will lag badly while the basket runs away, and it will be
flat during some of the best weeks. Historically it earns its keep in years
like 2022 (book −10%, coins −60%) and gives most of the bull years away. If
you cannot watch a benchmark triple while you make 20%, this strategy will get
overridden by hand — and a strategy that gets overridden has no properties at
all.

Rhythm: about **two trades per slice per month**. Most days, nothing happens.
That is the system working.

## What we do NOT claim

- **We do not claim it beats holding on raw profit.** Over any window
  containing a crypto mania, holding wins. The claim is about the *path*:
  similar destination, far shallower valleys.
- **The "extra return per unit of risk" is not statistically established.**
  Every excess figure we've measured sits inside its own error bar. The
  drawdown reduction is the believable part, because it's driven by
  diversification and sizing — arithmetic — rather than by any forecast.
- **The performance table above is descriptive, not proof.** That period was
  read many times during research (the contamination is logged in
  `experiments/holdout.json`). The honest test is pre-registered and forward:
  the live book itself, judged after **2027-08-25**, primary criterion
  *worst loss ≤ half the basket's*. Until then, short-term P&L means nothing —
  the error bars are wider than a year of results.

## Where it runs

On a Raspberry Pi (see [deploy/pi/README.md](../deploy/pi/README.md)), live since
2026-08-25, supervised by systemd, health-checked every 10 minutes. It was
moved off a laptop after macOS put the whole book to sleep for 50 minutes —
the machine matters less than the fact that it never sleeps.
