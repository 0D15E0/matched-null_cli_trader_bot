#!/usr/bin/env python3
"""Explore the causal inverse-volatility ensemble on fixed development data.

This is an exploratory grid, not a deployment selector. The date boundary,
universe, strategy rule, and candidate grid are compiled into the script so a
result cannot be extended into the frozen holdout by a command-line typo.
Risk windows are expressed in calendar days and converted to bars per
timeframe, making a 15-day volatility memory mean the same thing at 30m, 1h,
2h, 4h, and 1d.
"""

import argparse
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DEFAULT_BINARY = ROOT / "build" / "cli_trader"
DEFAULT_OUTPUT = Path("/tmp/ensemble_invvol_timeframe_sweep.json")
DATA_DIR = "data/derived"
DEVELOPMENT_START = "2021-01-01"
DEVELOPMENT_END = "2023-12-31"
WARMUP_BARS = 240
SYMBOLS = ("BTC_USDT", "ETH_USDT", "XRP_USDT", "LTC_USDT", "DOGE_USDT", "TRX_USDT", "ADA_USDT", "SOL_USDT")
TIMEFRAMES = {
    "30m": (1800, 48),
    "1h": (3600, 24),
    "2h": (7200, 12),
    "4h": (14400, 6),
    "1d": (86400, 1),
}
MEMORIES = (
    ("short", 7, 3),
    ("live", 15, 5),
    ("twenty", 20, 5),
    ("long", 30, 10),
)
VOL_TARGETS = (0.20, 0.30)


def parse_metrics(output: str) -> dict:
    common = re.search(r"^Common window:\s+(\d+) bars,\s+([-+0-9.]+) years", output, re.MULTILINE)
    sharpe = re.search(
        r"^Sharpe \(ann\.\):\s+([-+0-9.]+)\s+[-+0-9.]+\s+([-+0-9.]+)\s*$",
        output,
        re.MULTILINE,
    )
    error = re.search(r"^\s*\+/-\s+([-+0-9.]+)", output, re.MULTILINE)
    drawdown = re.search(r"^Max drawdown:\s+([-+0-9.]+)", output, re.MULTILINE)
    if not common or not sharpe or not error or not drawdown:
        raise ValueError("could not parse ensemble portfolio metrics")
    return {
        "common_bars": int(common.group(1)),
        "common_years": float(common.group(2)),
        "portfolio_sharpe": float(sharpe.group(1)),
        "excess_sharpe": float(sharpe.group(2)),
        "sharpe_standard_error": float(error.group(1)),
        "max_drawdown_pct": float(drawdown.group(1)),
    }


def run_candidate(binary: Path, label: str, period: int, bars_per_day: int,
                  memory_name: str, memory_days: int, rebalance_days: int,
                  vol_target: float) -> dict:
    sizing_window = max(10, round(15 * bars_per_day))
    vol_lookback = max(10, round(memory_days * bars_per_day))
    rebalance = max(1, round(rebalance_days * bars_per_day))
    envs = ",".join(f"{symbol}:{period}" for symbol in SYMBOLS)
    command = [
        str(binary),
        "portfolio",
        "--data-dir",
        DATA_DIR,
        "--envs",
        envs,
        "--start",
        DEVELOPMENT_START,
        "--end",
        DEVELOPMENT_END,
        "--warmup-bars",
        str(WARMUP_BARS),
        "--strategy",
        "ensemble_vote",
        "--sparams",
        "enterVotes=2,exitVotes=0",
        "--vol-target",
        str(vol_target),
        "--vol-window",
        str(sizing_window),
        "--weights",
        "invvol",
        "--vol-lookback",
        str(vol_lookback),
        "--rebalance",
        str(rebalance),
    ]
    completed = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    result = {
        "id": f"ensemble-invvol-{label}-{memory_name}-vt{vol_target:.2f}",
        "timeframe": label,
        "period": period,
        "memory_days": memory_days,
        "rebalance_days": rebalance_days,
        "sizing_vol_window_bars": sizing_window,
        "portfolio_vol_lookback_bars": vol_lookback,
        "portfolio_rebalance_bars": rebalance,
        "vol_target": vol_target,
        "command": command,
        "returncode": completed.returncode,
    }
    if completed.returncode != 0:
        result["error"] = completed.stderr.strip() or completed.stdout.strip()
        return result
    try:
        result.update(parse_metrics(completed.stdout))
    except ValueError as error:
        result["error"] = str(error)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    if not args.binary.exists():
        print(f"binary not found: {args.binary}", file=sys.stderr)
        return 2

    results = []
    for label, (period, bars_per_day) in TIMEFRAMES.items():
        for memory_name, memory_days, rebalance_days in MEMORIES:
            for vol_target in VOL_TARGETS:
                print(
                    f"running {label} memory={memory_days}d/{rebalance_days}d "
                    f"target={vol_target:g}",
                    file=sys.stderr,
                )
                results.append(
                    run_candidate(
                        args.binary,
                        label,
                        period,
                        bars_per_day,
                        memory_name,
                        memory_days,
                        rebalance_days,
                        vol_target,
                    )
                )

    successful = [result for result in results if "excess_sharpe" in result]
    failed = [result for result in results if "error" in result]
    successful.sort(key=lambda result: result["excess_sharpe"], reverse=True)
    payload = {
        "status": "development-only-exploration",
        "holdout_accessed": False,
        "development_start": DEVELOPMENT_START,
        "development_end": DEVELOPMENT_END,
        "warmup_bars": WARMUP_BARS,
        "symbols": SYMBOLS,
        "strategy": "ensemble_vote enterVotes=2 exitVotes=0",
        "weighting": "causal inverse volatility",
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "ranking": successful,
        "failed": failed,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2) + "\n")

    print(f"wrote {args.output}")
    print("rank timeframe memory target excess_sharpe sharpe_se max_dd_pct bars")
    for rank, result in enumerate(successful, start=1):
        print(
            f"{rank:4d} {result['timeframe']:9s} {result['memory_days']:2d}d/"
            f"{result['rebalance_days']:2d}d {result['vol_target']:6.2f} "
            f"{result['excess_sharpe']:13.2f} {result['sharpe_standard_error']:9.2f} "
            f"{result['max_drawdown_pct']:10.2f} {result['common_bars']:5d}"
        )
    if failed:
        print(f"failed candidates: {len(failed)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())