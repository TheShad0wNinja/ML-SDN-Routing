#!/usr/bin/env python3
"""Root aggregator for hierarchical SDN federated learning.

Phase 1 of the architecture: workers each run an ns-3 simulation of one
network section with their own RL agent. Every K training steps they
torch-save their actor state dict (the critic is LOCAL and never federated —
its Q-scale is topology-specific) as
    {dir}/worker_{worker_id}_round_{r}.pt
and block on the appearance of
    {dir}/global_round_{r}.pt
This script is the "Root Controller's ML Aggregator hat": it watches the
directory, FedAvgs the weights as soon as all M workers have submitted,
publishes the averaged model, and cleans up. No network sockets, no shared
RAM — the directory is the entire communication bus, which mirrors how a
real distributed SDN controller would push small payloads to a root via REST.

Crash resilience:
  - Worker dies: this script hits --round-timeout-s and drops the round.
  - Aggregator dies: workers eventually hit NS3_FEDAVG_WAIT_TIMEOUT_S and
    keep training with their pre-round local weights. On restart, the
    aggregator resumes at max(existing global_round_*) + 1.

Usage:
    python3 root_aggregator.py \
        --dir scratch/data/federated_weights \
        --num-workers 3 \
        --fedavg-every-steps 200
"""
from __future__ import annotations

import argparse
import csv
import os
import math
import re
import sys
import time

import torch


_WORKER_RE = re.compile(r"^worker_(\d+)_round_(\d+)\.pt$")
_GLOBAL_RE = re.compile(r"^global_round_(\d+)\.pt$")
_ARCH_TAG_DEFAULT = "gnn-v4"


def _find_workers_for_round(d: str, r: int) -> dict[int, str]:
    """Scan the directory and return {worker_id: file_path} for this round."""
    out: dict[int, str] = {}
    try:
        entries = os.listdir(d)
    except FileNotFoundError:
        return out
    for name in entries:
        m = _WORKER_RE.match(name)
        if not m:
            continue
        wid, rd = int(m.group(1)), int(m.group(2))
        if rd == r:
            out[wid] = os.path.join(d, name)
    return out


def _detect_next_round(d: str) -> int:
    """Resume after a restart by scanning for the highest already-published
    global file. Returns the round number to process next."""
    rs: list[int] = []
    try:
        entries = os.listdir(d)
    except FileNotFoundError:
        return 0
    for name in entries:
        m = _GLOBAL_RE.match(name)
        if m:
            rs.append(int(m.group(1)))
    return max(rs) + 1 if rs else 0


def _average_state_dicts(sds: list[dict]) -> dict:
    """Element-wise mean across a list of torch state_dicts.

    All input dicts must share keys and tensor shapes. The GAT actor/critic
    in ml_service.py is intentionally topology-invariant so this holds
    across sections of different sizes.
    """
    keys = list(sds[0].keys())
    avg = {}
    for k in keys:
        stacked = torch.stack([sd[k].float() for sd in sds], dim=0)
        avg[k] = stacked.mean(dim=0)
    return avg


def _l2(sd: dict) -> float:
    return math.sqrt(sum(t.float().pow(2).sum().item() for t in sd.values()))


def _shapes_match(sds: list[dict]) -> bool:
    ref = {k: tuple(v.shape) for k, v in sds[0].items()}
    for sd in sds[1:]:
        if {k: tuple(v.shape) for k, v in sd.items()} != ref:
            return False
    return True


def _wait_for_round(
    d: str,
    r: int,
    num_workers: int,
    timeout_s: float,
    poll_s: float = 0.5,
) -> tuple[dict[int, str], float, bool]:
    """Block until num_workers files exist for round r, or timeout elapses
    after the first file appears. Returns (workers_map, wait_seconds,
    dropped_due_to_timeout)."""
    t_start = time.perf_counter()
    t_first_seen: float | None = None
    while True:
        workers = _find_workers_for_round(d, r)
        if workers and t_first_seen is None:
            t_first_seen = time.perf_counter()
        if len(workers) >= num_workers:
            return workers, time.perf_counter() - t_start, False
        if (t_first_seen is not None
                and time.perf_counter() - t_first_seen > timeout_s):
            return workers, time.perf_counter() - t_start, True
        time.sleep(poll_s)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dir", required=True,
                   help="shared directory the workers write into")
    p.add_argument("--num-workers", type=int, required=True,
                   help="number of workers that must submit before averaging")
    p.add_argument("--arch-tag", default=_ARCH_TAG_DEFAULT,
                   help="enforce this arch tag on incoming worker files")
    p.add_argument("--round-timeout-s", type=float, default=300.0,
                   help="drop the round if not all M submit within this many "
                        "seconds of the first submit arriving")
    p.add_argument("--log-dir", default="scratch/data/fedavg",
                   help="where to write rounds.csv")
    p.add_argument("--clean-up", default="true",
                   help="delete worker_*_round_R.pt files after publishing "
                        "the round's global file (true|false)")
    args = p.parse_args()

    cleanup = str(args.clean_up).strip().lower() in ("1", "true", "yes", "on")
    os.makedirs(args.dir, exist_ok=True)
    os.makedirs(args.log_dir, exist_ok=True)
    rounds_csv = os.path.join(args.log_dir, "rounds.csv")
    fresh = not os.path.exists(rounds_csv)
    rfh = open(rounds_csv, "a", newline="")
    rw = csv.writer(rfh)
    if fresh:
        rw.writerow(["round", "n_submits", "actor_l2", "critic_l2",
                     "wait_s", "wall_ms", "dropped"])
        rfh.flush()

    next_round = _detect_next_round(args.dir)
    print(f"[AGG] start dir={args.dir} workers={args.num_workers} "
          f"first_round={next_round} timeout={args.round_timeout_s:.0f}s "
          f"cleanup={cleanup}")

    try:
        while True:
            workers, wait_s, dropped = _wait_for_round(
                args.dir, next_round, args.num_workers, args.round_timeout_s)

            if dropped:
                print(f"[AGG] round {next_round} dropped after {wait_s:.1f}s "
                      f"({len(workers)}/{args.num_workers} submits)")
                rw.writerow([next_round, len(workers), 0.0, 0.0,
                             f"{wait_s:.3f}", 0.0, 1])
                rfh.flush()
                next_round += 1
                continue

            t_collect_done = time.perf_counter()

            # Load all M blobs.
            blobs: list[dict] = []
            bad = False
            for wid in sorted(workers):
                path = workers[wid]
                try:
                    blob = torch.load(path, map_location="cpu",
                                      weights_only=False)
                except Exception as exc:
                    print(f"[AGG] failed to load {path}: {exc} — dropping round.")
                    bad = True
                    break
                if blob.get("arch") != args.arch_tag:
                    print(f"[AGG] worker {wid} arch="
                          f"{blob.get('arch')!r} != {args.arch_tag!r} — "
                          f"dropping round.")
                    bad = True
                    break
                # Only the actor is federated (localized critic): a worker
                # blob carries the actor alone. A legacy blob may still have a
                # critic key — harmless, it is ignored.
                if "actor" not in blob:
                    print(f"[AGG] worker {wid} missing actor — "
                          f"dropping round.")
                    bad = True
                    break
                blobs.append(blob)

            if bad:
                rw.writerow([next_round, len(workers), 0.0, 0.0,
                             f"{wait_s:.3f}", 0.0, 1])
                rfh.flush()
                next_round += 1
                continue

            # Shape check — the architectural invariant we rely on. Only the
            # actor is federated, so only the actor needs matching shapes.
            if not _shapes_match([b["actor"] for b in blobs]):
                print(f"[AGG] round {next_round}: worker tensor shapes "
                      f"disagree — federation cannot proceed. Dropping round.")
                rw.writerow([next_round, len(workers), 0.0, 0.0,
                             f"{wait_s:.3f}", 0.0, 1])
                rfh.flush()
                next_round += 1
                continue

            # Average the actor only (localized critic).
            actor_avg = _average_state_dicts([b["actor"] for b in blobs])
            actor_l2 = _l2(actor_avg)

            # Publish — atomic write so a worker mid-poll can't read a
            # half-written file. The global model is actor-only; workers load
            # just the actor and keep their own critic.
            out_path = os.path.join(args.dir, f"global_round_{next_round}.pt")
            tmp = out_path + ".tmp"
            torch.save({
                "arch": args.arch_tag,
                "round": next_round,
                "n_submits": len(workers),
                "actor": actor_avg,
            }, tmp)
            os.replace(tmp, out_path)

            wall_ms = (time.perf_counter() - t_collect_done) * 1000.0

            # Clean up worker files. Workers are still polling for the
            # global file, but won't read the worker_*.pt files themselves.
            if cleanup:
                for path in workers.values():
                    try:
                        os.remove(path)
                    except OSError:
                        pass

            print(f"[AGG] round {next_round} ok "
                  f"(submits={len(workers)} wait={wait_s:.1f}s "
                  f"wall={wall_ms:.0f}ms actor_l2={actor_l2:.4f})")
            # critic_l2 column kept for header stability but is empty: the
            # critic is no longer federated, so there is no global critic.
            rw.writerow([next_round, len(workers),
                         f"{actor_l2:.6f}", "",
                         f"{wait_s:.3f}", f"{wall_ms:.3f}", 0])
            rfh.flush()
            next_round += 1
    except KeyboardInterrupt:
        print("[AGG] shutting down.")
    finally:
        rfh.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
