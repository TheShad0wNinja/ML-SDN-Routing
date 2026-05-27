#!/usr/bin/env python3
"""Aggregate the per-tick training metrics into one summary row.

Reads scratch/data/agent/metrics.csv (written by ml_service.py's
_log_metrics) and prints the four headline numbers used by run_tests.sh's
summarize_log() to populate the ml_* columns of summary.csv.

Output format is `KEY=value` lines so the shell grep stays trivial:

  ML_TICKS=472
  ML_REWARD_FINAL=0.318
  ML_REWARD_MEAN_LAST25=0.291
  ML_CRITIC_LOSS_FINAL=0.043
  ML_ACTOR_LOSS_FINAL=-0.117

Empty cells (None) — typical of the pre-warmup ticks — are skipped when
computing loss aggregates. Final values are taken from the last row that
actually had a finite number, not the very last row, so warmup-induced
blanks at the tail don't poison the report.

Run with no args to point at the default path; pass a path to override
(e.g. for one-off analysis on an archived metrics.csv).
"""

import csv
import math
import sys
from pathlib import Path

DEFAULT_PATH = Path("scratch/data/agent/metrics.csv")


def _to_float(s: str):
    if s == "" or s is None:
        return None
    try:
        v = float(s)
        return v if math.isfinite(v) else None
    except ValueError:
        return None


def aggregate(path: Path) -> dict:
    rewards: list[float] = []
    critic: list[float] = []
    actor: list[float] = []
    n_ticks = 0
    if not path.exists():
        return {"ML_TICKS": 0}
    with path.open() as f:
        for row in csv.DictReader(f):
            n_ticks += 1
            r = _to_float(row.get("reward", ""))
            if r is not None:
                rewards.append(r)
            cl = _to_float(row.get("critic_loss", ""))
            if cl is not None:
                critic.append(cl)
            al = _to_float(row.get("actor_loss", ""))
            if al is not None:
                actor.append(al)

    out: dict[str, object] = {"ML_TICKS": n_ticks}
    if rewards:
        out["ML_REWARD_FINAL"] = rewards[-1]
        tail = rewards[-max(1, len(rewards) // 4):]
        out["ML_REWARD_MEAN_LAST25"] = sum(tail) / len(tail)
    if critic:
        out["ML_CRITIC_LOSS_FINAL"] = critic[-1]
    if actor:
        out["ML_ACTOR_LOSS_FINAL"] = actor[-1]
    return out


def main(argv: list[str]) -> int:
    path = Path(argv[1]) if len(argv) > 1 else DEFAULT_PATH
    agg = aggregate(path)
    for k, v in agg.items():
        if isinstance(v, float):
            print(f"{k}={v:.6f}")
        else:
            print(f"{k}={v}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
