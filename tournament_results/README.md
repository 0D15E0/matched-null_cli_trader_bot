# Tournament results

Raw output of the twelve `tournament` runs analysed in
[../docs/TOURNAMENT.md](../docs/TOURNAMENT.md). Each `<name>.json` carries the full
generation history, every tuned family champion with its parameters and
per-environment holdout report, and a `transfer` block with the rank-transfer
diagnostics. `<name>.log` is the console output of the same run.

All were produced with:

```sh
./build/cli_trader tournament --envs <ENVS> \
    --population 260 --generations 20 --tune-generations 12 \
    --seeds-per-family 5 --survive-threshold 0.0 --train-frac <FRAC> \
    --jobs 8 --save tournament_results/<name>.json
```

| file | `--envs` | `--train-frac` |
|---|---|---|
| `crypto4h_f50/60/70/80` | `BTC_USDT:14400,ETH_USDT:14400,LTC_USDT:14400,XRP_USDT:14400` | 0.50 / 0.60 / 0.70 / 0.80 |
| `crypto15m_f70` | `BTC_USDT:900,ETH_USDT:900` | 0.70 |
| `etf_daily_f60/f70` | `SPY:86400,QQQ:86400,GLD:86400,TLT:86400,EEM:86400` | 0.60 / 0.70 |
| `sectors_f70` | `XLK:86400,XLE:86400,XLF:86400,XLU:86400,XLV:86400` | 0.70 |
| `equities_f70` | `AAPL:86400,META:86400,AMZN:86400,TSLA:86400` | 0.70 |
| `crossdomain_f70` | `BTC_USDT:14400,SPY:86400,AAPL:86400,GLD:86400` | 0.70 |
| `btc_alone_f70` | `BTC_USDT:14400` | 0.70 |
| `eth_alone_f70` | `ETH_USDT:14400` | 0.70 |

`matched_null_btc_holdout.txt` is the exposure-matched control sweep from
TOURNAMENT.md section 3: 200 seeds of `control_random` at `entryProb=0.026,
holdBars=39, --vol-target 0.10` on the BTC holdout window (`--start
1678377600`), one row per seed as `excessSharpe timeInMarket% numTrades`.
Regenerate with:

```sh
for s in $(seq 1 200); do
  ./build/cli_trader backtest --symbol BTC_USDT --period 14400 \
      --strategy control_random --sparams "entryProb=0.026,holdBars=39,seed=$s" \
      --vol-target 0.10 --start 1678377600
done
```

**These are measurements of a search, not trading recommendations.** Every
finalist in them failed out-of-sample validation; see TOURNAMENT.md.
