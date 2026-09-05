#!/usr/bin/env python3
"""Register and evaluate trading hypotheses under a fixed research protocol.

The registry separates candidate generation from candidate evaluation. A
candidate is created with its rule, parameters, metric, and kill rules. It may
be run once on the fixed development folds. The result is recorded and the
candidate is permanently classified as either killed or surviving development.
No command in this file accepts an evaluation date, so it cannot accidentally
consume the frozen holdout or the live-forward period.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DEFAULT_REGISTRY = HERE / "hypotheses.json"
DEFAULT_BINARY = ROOT / "build" / "cli_trader"
DEVELOPMENT_END = "2023-12-31"
ENVIRONMENTS = "BTC_USDT:14400,ETH_USDT:14400,XRP_USDT:14400,LTC_USDT:14400"
WARMUP_BARS = 80
FOLDS = (
    ("2018-01-01", "2019-12-31"),
    ("2020-01-01", "2021-12-31"),
    ("2022-01-01", "2023-12-31"),
)
ID_PATTERN = re.compile(r"^[a-z0-9][a-z0-9_-]{2,63}$")
REGISTERED_STRATEGIES = {
    "odiseo", "sma_cross", "pure_ichimoku", "tsmom", "darvas", "pencil_extrap",
    "patterns", "fib_ichimoku", "faber_ma", "kama_trend", "ehlers_trend", "macd_norm",
    "donchian", "squeeze_breakout", "rsi_reversal", "ou_score", "skew_reversal",
    "fracdiff", "cusum_tb", "vol_managed", "vr_switch", "lppls_bubble", "ensemble_vote",
    "momo_breakout", "ensemble_ls", "ensemble_rule", "ensemble_tuned", "ensemble_asym",
    "dip_reversion", "ichimoku_full", "factor_trend", "control_random", "control_always_long",
}


def now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def load_registry(path: Path) -> dict:
    registry = json.loads(path.read_text())
    if registry.get("development_end") != DEVELOPMENT_END:
        raise ValueError("registry development boundary does not match the frozen protocol")
    if tuple(tuple(fold) for fold in registry.get("folds", ())) != FOLDS:
        raise ValueError("registry folds do not match the frozen protocol")
    for spec in registry.get("hypotheses", []):
        model = spec.setdefault("model", {})
        model.setdefault("sparams", "")
        model.setdefault("weights", "equal")
        model.setdefault("vol_lookback", 120)
        model.setdefault("rebalance", 30)
        model.setdefault("hurst_filter", None)
        model.setdefault("hurst_estimator", "sf")
        model.setdefault("hurst_window", 100)
    return registry


def save_registry(path: Path, registry: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent, text=True)
    try:
        with os.fdopen(fd, "w") as output:
            json.dump(registry, output, indent=2)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def validate_spec(spec: dict) -> list[str]:
    errors = []
    if not isinstance(spec.get("id"), str) or not ID_PATTERN.fullmatch(spec["id"]):
        errors.append("id must match [a-z0-9][a-z0-9_-]{2,63}")
    if not isinstance(spec.get("hypothesis"), str) or len(spec["hypothesis"].strip()) < 20:
        errors.append("hypothesis must contain the exact claim and be at least 20 characters")

    model = spec.get("model", {})
    if model.get("strategy") not in REGISTERED_STRATEGIES:
        errors.append("model.strategy must be a registered strategy family")
    if not isinstance(model.get("sparams", ""), str):
        errors.append("model.sparams must be a comma-separated name=value string")
    if not isinstance(model.get("vol_target"), (int, float)) or not 0.0 < model["vol_target"] <= 1.0:
        errors.append("model.vol_target must be in (0, 1]")
    if model.get("weights") not in {"equal", "invvol"}:
        errors.append("model.weights must be 'equal' or 'invvol'")
    if not isinstance(model.get("vol_lookback"), int) or model["vol_lookback"] < 10:
        errors.append("model.vol_lookback must be at least 10")
    if not isinstance(model.get("rebalance"), int) or model["rebalance"] < 1:
        errors.append("model.rebalance must be at least 1")

    hurst_filter = model.get("hurst_filter")
    if hurst_filter is not None and (not isinstance(hurst_filter, (int, float)) or not 0.0 < hurst_filter < 1.0):
        errors.append("model.hurst_filter must be in (0, 1) when enabled")
    if model.get("hurst_estimator") not in {"sf", "rs"}:
        errors.append("model.hurst_estimator must be 'sf' or 'rs'")
    if not isinstance(model.get("hurst_window"), int) or model["hurst_window"] < 20:
        errors.append("model.hurst_window must be at least 20")

    metric = spec.get("primary_metric")
    if metric != "mean_excess_sharpe":
        errors.append("primary_metric must be mean_excess_sharpe")

    rules = spec.get("kill_rules", {})
    if not isinstance(rules.get("min_positive_folds"), int) or not 1 <= rules["min_positive_folds"] <= len(FOLDS):
        errors.append("kill_rules.min_positive_folds must be between 1 and 3")
    if not isinstance(rules.get("min_mean_excess_sharpe"), (int, float)):
        errors.append("kill_rules.min_mean_excess_sharpe is required")
    if not isinstance(rules.get("min_worst_excess_sharpe"), (int, float)):
        errors.append("kill_rules.min_worst_excess_sharpe is required")
    if not isinstance(rules.get("max_worst_drawdown_pct"), (int, float)) or rules["max_worst_drawdown_pct"] <= 0:
        errors.append("kill_rules.max_worst_drawdown_pct must be positive")
    return errors


def parse_metrics(output: str) -> dict:
    sharpe = re.search(
        r"^Sharpe \(ann\.\):\s+([-+0-9.]+)\s+[-+0-9.]+\s+([-+0-9.]+)\s*$",
        output,
        re.MULTILINE,
    )
    error = re.search(r"^\s*\+/-\s+([-+0-9.]+)", output, re.MULTILINE)
    drawdown = re.search(r"^Max drawdown:\s+([-+0-9.]+)", output, re.MULTILINE)
    if not sharpe or not error or not drawdown:
        raise ValueError("could not parse portfolio metrics")
    return {
        "portfolio_sharpe": float(sharpe.group(1)),
        "excess_sharpe": float(sharpe.group(2)),
        "sharpe_standard_error": float(error.group(1)),
        "portfolio_max_drawdown_pct": float(drawdown.group(1)),
    }


def run_fold(binary: Path, spec: dict, start: str, end: str) -> dict:
    model = spec["model"]
    command = [
        str(binary),
        "portfolio",
        "--envs",
        ENVIRONMENTS,
        "--start",
        start,
        "--end",
        end,
        "--warmup-bars",
        str(WARMUP_BARS),
        "--strategy",
        model["strategy"],
        "--vol-target",
        str(model["vol_target"]),
        "--weights",
        model["weights"],
        "--vol-lookback",
        str(model["vol_lookback"]),
        "--rebalance",
        str(model["rebalance"]),
    ]
    if model["sparams"]:
        command.extend(["--sparams", model["sparams"]])
    if model["hurst_filter"] is not None:
        command.extend([
            "--hurst-filter",
            str(model["hurst_filter"]),
            "--hurst-estimator",
            model["hurst_estimator"],
            "--hurst-window",
            str(model["hurst_window"]),
        ])
    completed = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    result = {
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


def classify(results: list[dict], rules: dict) -> tuple[str, list[str], dict]:
    failures = [result for result in results if "error" in result]
    if failures:
        return "killed", ["one or more folds failed to execute"], {"failed_folds": len(failures)}

    excess = [result["excess_sharpe"] for result in results]
    drawdowns = [result["portfolio_max_drawdown_pct"] for result in results]
    summary = {
        "folds": len(results),
        "mean_excess_sharpe": sum(excess) / len(excess),
        "worst_excess_sharpe": min(excess),
        "positive_folds": sum(value > 0.0 for value in excess),
        "worst_max_drawdown_pct": max(drawdowns),
    }
    reasons = []
    if summary["positive_folds"] < rules["min_positive_folds"]:
        reasons.append("positive fold count below kill threshold")
    if summary["mean_excess_sharpe"] < rules["min_mean_excess_sharpe"]:
        reasons.append("mean excess Sharpe below kill threshold")
    if summary["worst_excess_sharpe"] < rules["min_worst_excess_sharpe"]:
        reasons.append("worst-fold excess Sharpe below kill threshold")
    if summary["worst_max_drawdown_pct"] > rules["max_worst_drawdown_pct"]:
        reasons.append("worst drawdown above kill threshold")
    return ("killed" if reasons else "survives_development", reasons, summary)


def find_spec(registry: dict, candidate_id: str) -> dict:
    for spec in registry["hypotheses"]:
        if spec["id"] == candidate_id:
            return spec
    raise ValueError(f"unknown hypothesis id: {candidate_id}")


def command_new(args: argparse.Namespace) -> int:
    registry = load_registry(args.registry)
    if any(spec["id"] == args.id for spec in registry["hypotheses"]):
        raise ValueError(f"hypothesis already exists: {args.id}")
    spec = {
        "id": args.id,
        "status": "proposed",
        "created_at": now(),
        "hypothesis": args.hypothesis,
        "primary_metric": "mean_excess_sharpe",
        "model": {
            "strategy": args.strategy,
            "sparams": args.sparams,
            "vol_target": args.vol_target,
            "weights": args.weights,
            "vol_lookback": args.vol_lookback,
            "rebalance": args.rebalance,
            "hurst_filter": args.hurst_filter,
            "hurst_estimator": args.hurst_estimator,
            "hurst_window": args.hurst_window,
        },
        "kill_rules": {
            "min_positive_folds": args.min_positive_folds,
            "min_mean_excess_sharpe": args.min_mean_excess,
            "min_worst_excess_sharpe": args.min_worst_excess,
            "max_worst_drawdown_pct": args.max_worst_drawdown,
        },
        "runs": [],
    }
    errors = validate_spec(spec)
    if errors:
        raise ValueError("invalid hypothesis: " + "; ".join(errors))
    registry["hypotheses"].append(spec)
    save_registry(args.registry, registry)
    print(f"registered proposed hypothesis: {args.id}")
    return 0


def command_validate(args: argparse.Namespace) -> int:
    registry = load_registry(args.registry)
    spec = find_spec(registry, args.id)
    errors = validate_spec(spec)
    if errors:
        print("INVALID")
        for error in errors:
            print(f"- {error}")
        return 1
    print(f"VALID {args.id} ({spec['status']})")
    return 0


def command_run(args: argparse.Namespace) -> int:
    registry = load_registry(args.registry)
    spec = find_spec(registry, args.id)
    errors = validate_spec(spec)
    if errors:
        raise ValueError("invalid hypothesis: " + "; ".join(errors))
    if spec["status"] != "proposed":
        raise ValueError(f"{args.id} is {spec['status']}; proposed hypotheses can run once")
    if not args.binary.exists():
        raise ValueError(f"binary not found: {args.binary}")

    results = []
    for start, end in FOLDS:
        print(f"running {args.id}: {start}..{end}", file=sys.stderr)
        results.append(run_fold(args.binary, spec, start, end))
    status, reasons, summary = classify(results, spec["kill_rules"])
    spec["runs"].append({
        "completed_at": now(),
        "status": status,
        "reasons": reasons,
        "summary": summary,
        "fold_results": results,
    })
    spec["status"] = status
    spec["decision_at"] = now()
    save_registry(args.registry, registry)
    print(json.dumps({"id": args.id, "status": status, "reasons": reasons, "summary": summary}, indent=2))
    return 0 if status != "killed" else 1


def command_list(args: argparse.Namespace) -> int:
    registry = load_registry(args.registry)
    print("id status strategy weights vol_target filter")
    for spec in registry["hypotheses"]:
        model = spec["model"]
        filter_name = "none" if model["hurst_filter"] is None else f"hurst>={model['hurst_filter']:.2f}"
        print(
            f"{spec['id']:32s} {spec['status']:22s} {model['strategy']:18s} "
            f"{model['weights']:7s} {model['vol_target']:.2f} {filter_name}"
        )
    return 0


def command_show(args: argparse.Namespace) -> int:
    registry = load_registry(args.registry)
    print(json.dumps(find_spec(registry, args.id), indent=2))
    return 0


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    root.add_argument("--registry", type=Path, default=DEFAULT_REGISTRY)
    commands = root.add_subparsers(dest="command", required=True)

    new = commands.add_parser("new", help="register a candidate without running it")
    new.add_argument("--id", required=True)
    new.add_argument("--hypothesis", required=True)
    new.add_argument("--strategy", required=True)
    new.add_argument("--sparams", default="")
    new.add_argument("--vol-target", type=float, required=True)
    new.add_argument("--weights", choices=("equal", "invvol"), default="equal")
    new.add_argument("--vol-lookback", type=int, default=120)
    new.add_argument("--rebalance", type=int, default=30)
    new.add_argument("--hurst-filter", type=float)
    new.add_argument("--hurst-estimator", choices=("sf", "rs"), default="sf")
    new.add_argument("--hurst-window", type=int, default=100)
    new.add_argument("--min-positive-folds", type=int, default=2)
    new.add_argument("--min-mean-excess", type=float, default=0.10)
    new.add_argument("--min-worst-excess", type=float, default=-0.20)
    new.add_argument("--max-worst-drawdown", type=float, default=40.0)
    new.set_defaults(function=command_new)

    for name, function in (("validate", command_validate), ("show", command_show), ("run", command_run)):
        command = commands.add_parser(name)
        command.add_argument("id")
        if name == "run":
            command.add_argument("--binary", type=Path, default=DEFAULT_BINARY)
        command.set_defaults(function=function)
    commands.add_parser("list").set_defaults(function=command_list)
    return root


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    return args.function(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(2)