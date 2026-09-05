#!/usr/bin/env python3
"""Evaluate a frozen portfolio shortlist on chronological development folds.

All folds end before the frozen 2024 boundary. The candidate set and fold
boundaries are part of this script, and the portfolio command receives an
explicit warm-up prefix so indicators are causal at every fold boundary.
This is stability evidence for development research, not holdout validation.
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
DEFAULT_ENVS = "BTC_USDT:14400,ETH_USDT:14400,XRP_USDT:14400,LTC_USDT:14400"
WARMUP_BARS = 80
FOLDS = (
    ("2018-01-01", "2019-12-31"),
    ("2020-01-01", "2021-12-31"),
    ("2022-01-01", "2023-12-31"),
)
CANDIDATES = (
    ("ensemble_vote", 0.10),
    ("ensemble_vote", 0.20),
    ("ensemble_vote", 0.30),
    ("donchian", 0.20),
    ("tsmom", 0.20),
    ("faber_ma", 0.20),
)


def parse_metrics(output: str) -> dict:
    sharpe = re.search(
        r"^Sharpe \(ann\.\):\s+([-+0-9.]+)\s+[-+0-9.]+\s+([-+0-9.]+)\s*$",
        output,
        re.MULTILINE,
    )
    error = re.search(r"^\s*\+/-\s+([-+0-9.]+)", output, re.MULTILINE)
    drawdown = re.search(r"^Max drawdown:\s+([-+0-9.]+)", output, re.MULTILINE)
    if not sharpe or not error or not drawdown:
        raise ValueError("could not parse portfolio walk-forward metrics")
    return {
        "portfolio_sharpe": float(sharpe.group(1)),
        "excess_sharpe": float(sharpe.group(2)),
        "sharpe_standard_error": float(error.group(1)),
        "portfolio_max_drawdown_pct": float(drawdown.group(1)),
    }


def run_fold(binary: Path, strategy: str, vol_target: float, start: str, end: str) -> dict:
    command = [
        str(binary),
        "portfolio",
        "--envs",
        DEFAULT_ENVS,
        "--start",
        start,
        "--end",
        end,
        "--warmup-bars",
        str(WARMUP_BARS),
        "--strategy",
        strategy,
        "--vol-target",
        str(vol_target),
        "--weights",
        "equal",
    ]
    completed = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    result = {
        "strategy": strategy,
        "vol_target": vol_target,
        "fold_start": start,
        "fold_end": end,
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
    parser.add_argument(
        "--output", type=Path, default=HERE / "results" / "dev_portfolio_walkforward.json"
    )
    args = parser.parse_args()

    if not args.binary.exists():
        print(f"binary not found: {args.binary}", file=sys.stderr)
        return 2

    folds = []
    for strategy, vol_target in CANDIDATES:
        for start, end in FOLDS:
            print(f"running {strategy} vt={vol_target:g} {start}..{end}", file=sys.stderr)
            folds.append(run_fold(args.binary, strategy, vol_target, start, end))

    successful = [result for result in folds if "excess_sharpe" in result]
    failed = [result for result in folds if "error" in result]
    aggregate = []
    for strategy, vol_target in CANDIDATES:
        rows = [
            result
            for result in successful
            if result["strategy"] == strategy and result["vol_target"] == vol_target
        ]
        if not rows:
            continue
        excess = [row["excess_sharpe"] for row in rows]
        aggregate.append(
            {
                "strategy": strategy,
                "vol_target": vol_target,
                "folds": len(rows),
                "mean_excess_sharpe": sum(excess) / len(excess),
                "worst_excess_sharpe": min(excess),
                "positive_folds": sum(value > 0.0 for value in excess),
                "mean_sharpe_standard_error": sum(
                    row["sharpe_standard_error"] for row in rows
                )
                / len(rows),
                "worst_max_drawdown_pct": max(
                    row["portfolio_max_drawdown_pct"] for row in rows
                ),
            }
        )
    aggregate.sort(
        key=lambda result: (result["mean_excess_sharpe"], result["worst_excess_sharpe"]),
        reverse=True,
    )

    payload = {
        "status": "development-walk-forward-stability",
        "holdout_accessed": False,
        "environments": DEFAULT_ENVS,
        "warmup_bars": WARMUP_BARS,
        "folds": FOLDS,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "aggregate": aggregate,
        "fold_results": successful,
        "failed": failed,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2) + "\n")

    print(f"wrote {args.output}")
    print("rank strategy vol_target mean_excess worst_excess positive_folds worst_dd")
    for rank, result in enumerate(aggregate, start=1):
        print(
            f"{rank:4d} {result['strategy']:16s} {result['vol_target']:10.2f} "
            f"{result['mean_excess_sharpe']:11.2f} {result['worst_excess_sharpe']:12.2f} "
            f"{result['positive_folds']:13d} {result['worst_max_drawdown_pct']:9.2f}"
        )
    if failed:
        print(f"failed folds: {len(failed)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())