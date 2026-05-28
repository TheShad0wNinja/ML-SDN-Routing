#!/usr/bin/env python3
"""Aggregate per-tick training metrics into one summary row.

Reads either:
  - a single metrics.csv file (single-worker / eval mode), or
  - a priority directory like scratch/data/agent/<priority>/ in which case
    all w*/metrics.csv files are merged across workers.

In train mode each worker has its own scratch/data/agent/<priority>/w<N>/
checkpoint dir with its own metrics.csv. This script discovers them
automatically and reports cross-worker means for the headline numbers.

Output format is `KEY=value` lines so the shell grep stays trivial:

  ML_TICKS=472
  ML_WORKERS=6
  ML_REWARD_FINAL=0.318
  ML_REWARD_MEAN_LAST25=0.291
  ML_CRITIC_LOSS_FINAL=0.043
  ML_ACTOR_LOSS_FINAL=-0.117

Empty cells (None) — typical of the pre-warmup ticks — are skipped when
computing aggregates. Per-worker "finals" are taken from the last row
with a finite value, then averaged across workers.

Run with no args to point at the default path; pass a path to override
(e.g. for one-off analysis on an archived metrics.csv or priority dir).
"""

import csv
import math
import sys
from pathlib import Path

# Default points at the priority dir, not a specific metrics.csv. The shell
# (summarize_log) passes the priority dir so the auto-discovery covers both
# single-worker and multi-worker (train) layouts.
DEFAULT_PATH = Path("scratch/data/agent/balanced")


def _to_float(s: str):
    if s == "" or s is None:
        return None
    try:
        v = float(s)
        return v if math.isfinite(v) else None
    except ValueError:
        return None


def find_metrics_csvs(path: Path) -> list[Path]:
    """If path is a file, return [path]. If a directory, find metrics.csv
    in the dir itself (single-worker layout) and in immediate w*/ subdirs
    (multi-worker / train layout). Keeps depth shallow so accidental sweeps
    of archived data don't get picked up."""
    if path.is_file():
        return [path]
    if not path.is_dir():
        return []
    found: list[Path] = []
    direct = path / "metrics.csv"
    if direct.exists():
        found.append(direct)
    for sub in sorted(path.glob("w*/metrics.csv")):
        found.append(sub)
    return found


def _summarize_worker(p: Path) -> dict:
    rewards: list[float] = []
    critic: list[float] = []
    actor: list[float] = []
    n_ticks = 0
    with p.open() as f:
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
    return {
        "ticks": n_ticks,
        "rewards": rewards,
        "critic": critic,
        "actor": actor,
    }


def aggregate(path: Path) -> dict:
    files = find_metrics_csvs(path)
    if not files:
        return {"ML_TICKS": 0}

    per_worker = [_summarize_worker(f) for f in files]
    out: dict[str, object] = {
        "ML_TICKS": sum(w["ticks"] for w in per_worker),
        "ML_WORKERS": len(per_worker),
    }

    # Per-worker "final" then mean across workers — robust to differing
    # finish times and silent on workers that never produced a finite value.
    finals = [w["rewards"][-1] for w in per_worker if w["rewards"]]
    if finals:
        out["ML_REWARD_FINAL"] = sum(finals) / len(finals)

    # Last 25% per worker, then mean. Captures convergence rather than
    # instantaneous noise.
    tails = []
    for w in per_worker:
        rs = w["rewards"]
        if rs:
            tail = rs[-max(1, len(rs) // 4):]
            tails.append(sum(tail) / len(tail))
    if tails:
        out["ML_REWARD_MEAN_LAST25"] = sum(tails) / len(tails)

    cl_finals = [w["critic"][-1] for w in per_worker if w["critic"]]
    if cl_finals:
        out["ML_CRITIC_LOSS_FINAL"] = sum(cl_finals) / len(cl_finals)
    al_finals = [w["actor"][-1] for w in per_worker if w["actor"]]
    if al_finals:
        out["ML_ACTOR_LOSS_FINAL"] = sum(al_finals) / len(al_finals)

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
