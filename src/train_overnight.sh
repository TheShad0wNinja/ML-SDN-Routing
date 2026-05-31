#!/usr/bin/env bash
# train_overnight.sh — one-shot federated training + zero-shot eval.
#
# SELF-DAEMONIZING. Just run it bare; it re-execs itself under nohup and
# returns immediately, printing the detached PID and a log path:
#
#     ./scratch/train_overnight.sh
#     # → [train_overnight] detached PID=12345
#     #   [train_overnight] log: /workspace/ns-3.40/train_overnight.<ts>.log
#
# The internal `--run` flag is what the detached copy uses to do the actual
# work — you never pass it yourself.
#
# WHAT IT DOES (the --run path):
#   1. Mixed-topology FEDERATED training across usa + fat-tree-k4 +
#      sensor-cluster, split evenly across --workers. FedAvg learns ONE global
#      actor; each worker keeps its own LOCAL critic (localized-critic design).
#   2. Zero-shot EVAL of that single federated actor on each topology.
#   3. Points you at the summary CSV / round log.
#
# Deliberately simple: ONE priority, no priority sweep, no policy-collapse
# validation. Tune via env vars (var = default):
#   PRIORITY=energy     ROUNDS=30       WORKERS=8
#   SEED=20042810         SIM_TIME=600    EVAL_SIM_TIME=300
#   MIX="usa,fat-tree-k4,sensor-cluster"  FEDAVG_EVERY=50   RESET=0  (1 = wipe)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RT="$SCRIPT_DIR/run_tests.sh"

# ---- self-daemonize ---------------------------------------------------------
# Without --run, fork a detached nohup copy and exit. With --run, fall through.
if [[ "${1:-}" != "--run" ]]; then
  ts="$(date +%Y%m%d-%H%M%S)"
  log="$REPO_ROOT/train_overnight.$ts.log"
  nohup "$0" --run "$@" >"$log" 2>&1 &
  pid=$!
  echo "[train_overnight] detached PID=$pid"
  echo "[train_overnight] log: $log"
  echo "[train_overnight] follow with:  tail -f \"$log\""
  echo "[train_overnight] stop   with:  kill $pid"
  exit 0
fi
shift  # drop the --run sentinel

cd "$REPO_ROOT"

PRIORITY="${PRIORITY:-energy}"
ROUNDS="${ROUNDS:-30}"
WORKERS="${WORKERS:-8}"
SEED="${SEED:-20042810}"
SIM_TIME="${SIM_TIME:-600}"
EVAL_SIM_TIME="${EVAL_SIM_TIME:-300}"
MIX="${MIX:-usa,fat-tree-k4,sensor-cluster}"
FEDAVG_EVERY="${FEDAVG_EVERY:-50}"
RESET="${RESET:-0}"

echo "[train_overnight] start $(date '+%Y-%m-%d %H:%M:%S')"
echo "[train_overnight]   priority=$PRIORITY rounds=$ROUNDS workers=$WORKERS mix='$MIX'"
echo "[train_overnight]   seed=$SEED simTime=$SIM_TIME evalSimTime=$EVAL_SIM_TIME reset=$RESET"

# 1. Mixed-topology federated training (one global actor; per-worker critics).
echo
echo "[train_overnight] ===== TRAIN (mixed federated) ====="
train_args=(train
  --mix "$MIX"
  --workers "$WORKERS"
  --priority "$PRIORITY"
  --rounds "$ROUNDS"
  --seed "$SEED"
  --simTime "$SIM_TIME"
  --fedAvgEverySteps "$FEDAVG_EVERY")
[[ "$RESET" == "1" ]] && train_args+=(--reset)
"$RT" "${train_args[@]}"

# 2. Zero-shot eval of the federated actor on each distinct topology in the mix.
IFS=',' read -ra _entries <<< "$MIX"
declare -A _seen=()
for entry in "${_entries[@]}"; do
  topo="${entry%%:*}"; topo="${topo// /}"
  [[ -z "$topo" || -n "${_seen[$topo]:-}" ]] && continue
  _seen[$topo]=1
  echo
  echo "[train_overnight] ===== EVAL $topo (priority=$PRIORITY) ====="
  "$RT" eval \
    --topology "$topo" \
    --priority "$PRIORITY" \
    --simTime "$EVAL_SIM_TIME" \
    --seed "$SEED" || echo "[train_overnight] WARN: eval $topo failed (continuing)"
done

# 3. Where to look.
echo
echo "[train_overnight] DONE $(date '+%Y-%m-%d %H:%M:%S')"
echo "[train_overnight]   summary CSV : $SCRIPT_DIR/data/results/summary.csv"
echo "[train_overnight]   run logs    : $SCRIPT_DIR/data/results/logs/"
echo "[train_overnight]   fedavg log  : $SCRIPT_DIR/data/fedavg/$PRIORITY/rounds.csv"
