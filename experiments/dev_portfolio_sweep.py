#!/usr/bin/env python3
"""Run a fixed, development-only portfolio comparison.

This script deliberately has no --end option. Its evaluation boundary is
part of the protocol, so expanding the search cannot accidentally consume the
holdout through a command-line typo.
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
DEVELOPMENT_END = "2023-12-31"
DEFAULT_STRATEGIES = (
    "tsmom",
    "ensemble_vote",
    "faber_ma",
    "donchian",
    "sma_cross",
    "fracdiff",
    "rsi_reversal",
)


def metric(output: str, label: str) -> float:
    match = re.search(rf"^{re.escape(label)}\s+([-+0-9.]+)", output, re.MULTILINE)
    if not match:
        raise ValueError(f"could not parse {label!r} from portfolio output")
    return float(match.group(1))


def sharpe_metrics(output: str) -> tuple[float, float, float]:
    match = re.search(
        r"^Sharpe \(ann\.\):\s+([-+0-9.]+)\s+[-+0-9.]+\s+([-+0-9.]+)\s*$",
        output,
        re.MULTILINE,
    )
    if not match:
        raise ValueError("could not parse portfolio Sharpe output")
    error_match = re.search(r"^\s*\+/-\s+([-+0-9.]+)", output, re.MULTILINE)
    if not error_match:
        raise ValueError("could not parse portfolio Sharpe standard error")
    return float(match.group(1)), float(match.group(2)), float(error_match.group(1))


def run_candidate(binary: Path, strategy: str, vol_target: float, weights: str) -> dict:
    command = [
        str(binary),
        "portfolio",
        "--envs",
        DEFAULT_ENVS,
        "--end",
        DEVELOPMENT_END,
        "--strategy",
        strategy,
        "--vol-target",
        str(vol_target),
        "--weights",
        weights,
    ]
    completed = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    result = {
        "strategy": strategy,
        "vol_target": vol_target,
        "weights": weights,
        "command": command,
        "returncode": completed.returncode,
    }
    if completed.returncode != 0:
        result["error"] = completed.stderr.strip() or completed.stdout.strip()
        return result

    try:
        portfolio_sharpe, excess_sharpe, sharpe_se = sharpe_metrics(completed.stdout)
        result.update(
            {
                "portfolio_return_pct": metric(completed.stdout, "Total return:"),
                "portfolio_cagr_pct": metric(completed.stdout, "CAGR:"),
                "portfolio_sharpe": portfolio_sharpe,
                "sharpe_standard_error": sharpe_se,
                "excess_sharpe": excess_sharpe,
                "portfolio_sortino": metric(completed.stdout, "Sortino (ann.):"),
                "portfolio_max_drawdown_pct": metric(completed.stdout, "Max drawdown:"),
            }
        )
    except ValueError as error:
        result["error"] = str(error)
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
    parser.add_argument("--output", type=Path, default=HERE / "results" / "dev_portfolio_sweep.json")
    parser.add_argument("--strategies", nargs="+", default=DEFAULT_STRATEGIES)
    parser.add_argument("--vol-targets", nargs="+", type=float, default=[0.10, 0.20, 0.30])
    parser.add_argument("--weights", nargs="+", choices=("equal", "invvol"), default=["equal"])
    args = parser.parse_args()

    if not args.binary.exists():
        print(f"binary not found: {args.binary}", file=sys.stderr)
        return 2

    results = []
    for strategy in args.strategies:
        for vol_target in args.vol_targets:
            for weights in args.weights:
                print(f"running {strategy} vt={vol_target:g} weights={weights}", file=sys.stderr)
                results.append(run_candidate(args.binary, strategy, vol_target, weights))

    successful = [result for result in results if "excess_sharpe" in result]
    successful.sort(
        key=lambda result: (
            result["excess_sharpe"],
            -result["portfolio_max_drawdown_pct"],
        ),
        reverse=True,
    )
    payload = {
        "status": "development-only",
        "evaluation_end": DEVELOPMENT_END,
        "environments": DEFAULT_ENVS,
        "holdout_accessed": False,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "ranking": successful,
        "failed": [result for result in results if "error" in result],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2) + "\n")

    print(f"wrote {args.output}")
    print("rank strategy vol_target weights excess_sharpe sharpe_se max_dd_pct")
    for rank, result in enumerate(successful, start=1):
        print(
            f"{rank:4d} {result['strategy']:16s} {result['vol_target']:10.2f} "
            f"{result['weights']:7s} {result['excess_sharpe']:13.2f} "
            f"{result['sharpe_standard_error']:9.2f} "
            f"{result['portfolio_max_drawdown_pct']:10.2f}"
        )
    if payload["failed"]:
        print(f"failed candidates: {len(payload['failed'])}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())