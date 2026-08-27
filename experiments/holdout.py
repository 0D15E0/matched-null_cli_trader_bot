#!/usr/bin/env python3
"""The frozen holdout boundary, and the only sanctioned way to ask for it.

WHY THIS FILE EXISTS
--------------------
By 2026-08-22 this project had tested six hypotheses against the full history
and falsified all six (logged in the project's research notes). The problem with
a seventh test is not that six failed - it is that every test on the same sample
consumes credibility that cannot be earned back by more testing. The order-gate
experiment measured exactly how much: a RANDOM ranking, evaluated at all seven
plausible selection sizes and scored at its best, reached the reported effect in
15.9% of draws. Search-adjusted, a "p = 0.001" became p = 0.159.

So the data is now split, and the split is frozen:

    development   everything BEFORE 2024-01-01     -- explore freely
    holdout       2024-01-01 onward                -- one look per hypothesis

USAGE

    from holdout import dev_range, holdout_range, register

    start, end = dev_range()            # (None, 1704067200) - safe, unlimited use
    ...develop and tune here...

    # only once you have written down the rule AND the statistic:
    start, end = holdout_range(hypothesis="cross-sectional momentum, top-3 of 8, "
                                          "90-bar lookback, judged on portfolio "
                                          "Sharpe vs equal-weight basket")

`holdout_range` refuses to answer without a hypothesis string, and appends what
you asked to holdout.json so the count of looks is a matter of record rather
than of memory. That log is the honest denominator for any p-value this project
ever reports again.
"""
import json
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
CONFIG = HERE / "holdout.json"


def _load():
    return json.loads(CONFIG.read_text())


def boundary():
    """Epoch seconds of the first holdout bar."""
    return int(_load()["holdout_start_epoch"])


def dev_range():
    """(start, end) for development. Unlimited use, no logging."""
    return None, boundary()


def holdout_range(hypothesis: str, note: str = ""):
    """(start, end) for the holdout. Requires a written hypothesis and logs the
    look. Not a technical lock - a paper trail. If you find yourself deleting
    entries from the log, the experiment is already over."""
    if not hypothesis or len(hypothesis.strip()) < 20:
        raise ValueError(
            "holdout_range requires a specific written hypothesis (>= 20 chars): "
            "the exact rule AND the exact statistic it will be judged on, decided "
            "BEFORE you look. Vague hypotheses are how a holdout becomes a second "
            "training set.")
    cfg = _load()
    cfg.setdefault("hypotheses_tested_on_holdout", []).append({
        "hypothesis": hypothesis.strip(),
        "note": note,
        "looked_at": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    })
    CONFIG.write_text(json.dumps(cfg, indent=2) + "\n")
    return boundary(), None


def looks_taken():
    return len(_load().get("hypotheses_tested_on_holdout", []))


if __name__ == "__main__":
    cfg = _load()
    b = boundary()
    print(f"holdout starts {cfg['holdout_start']}  (epoch {b}), frozen {cfg['frozen_on']}")
    print(f"development range: everything before {cfg['holdout_start']}")
    print(f"looks taken on the holdout: {looks_taken()}")
    for h in cfg.get("hypotheses_tested_on_holdout", []):
        print(f"  - [{h['looked_at']}] {h['hypothesis']}")
