#!/usr/bin/env bash
# benchmark.sh — compare ML vs baseline across all topologies.
#
# For each topology the ML sweep and the baseline sweep are launched in
# parallel (separate run_tests.sh invocations backgrounded and waited on).
# Within each sweep, --workers seeds run simultaneously — each ML seed gets
# its own Python service slot, exactly like federated training.
#
# Usage:
#   scratch/benchmark.sh [options]
#
# Options:
#   --priority X        balanced | throughput | energy | custom   (default: energy)
#   --workers N         parallel seeds per sweep                  (default: 3)
#   --seeds N           number of seeds per sweep                 (default: 3)
#   --seed N            base seed                                 (default: 12345)
#   --simTime N         simulation seconds                        (default: 700)
#   --trafficMode X     central | random | grouped                (default: central)
#   --topos "A B ..."   space-separated topology list             (default: all three)
#   -h|--help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_TESTS="$SCRIPT_DIR/run_tests.sh"

# ---- defaults ----------------------------------------------------------------
PRIORITY=energy
WORKERS=8
N_SEEDS=3
BASE_SEED=04041973
SIM_TIME=700
TRAFFIC_MODE=random
TOPOS=(usa fat-tree-k4 sensor-cluster geant)

# ---- arg parsing -------------------------------------------------------------
while [[ $# -gt 0 ]]; do
  case "$1" in
    --priority)    PRIORITY="$2";                   shift 2 ;;
    --workers)     WORKERS="$2";                    shift 2 ;;
    --seeds)       N_SEEDS="$2";                    shift 2 ;;
    --seed)        BASE_SEED="$2";                  shift 2 ;;
    --simTime)     SIM_TIME="$2";                   shift 2 ;;
    --trafficMode) TRAFFIC_MODE="$2";               shift 2 ;;
    --topos)       read -ra TOPOS <<< "$2";         shift 2 ;;
    -h|--help)
      grep '^#' "$0" | grep -v '^#!/' | sed 's/^# \?//'
      exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

# ---- helpers -----------------------------------------------------------------
topo_supports_cripple() { [[ "$1" == "usa" ]]; }

# Pad a string to a fixed width for aligned banners.
pad() { printf "%-${2}s" "$1"; }

# ---- top banner --------------------------------------------------------------
TOPOS_STR="${TOPOS[*]}"
PRI_UPPER="$(echo "$PRIORITY" | tr '[:lower:]' '[:upper:]')"
echo "╔══════════════════════════════════════════════════════════════════════╗"
printf "║  %-70s║\n" "BENCHMARK"
printf "║  %-70s║\n" ""
printf "║  %-70s║\n" "priority    :  $PRI_UPPER"
printf "║  %-70s║\n" "topologies  :  $TOPOS_STR"
printf "║  %-70s║\n" "seeds       :  $N_SEEDS  (base=$BASE_SEED)"
printf "║  %-70s║\n" "workers     :  $WORKERS per sweep  (ML + baseline run in parallel)"
printf "║  %-70s║\n" "simTime     :  ${SIM_TIME}s"
printf "║  %-70s║\n" "trafficMode :  $TRAFFIC_MODE"
printf "║  %-70s║\n" "failures    :  on (all topologies)"
printf "║  %-70s║\n" "cripple     :  on (usa only)"
printf "║  %-70s║\n" "mixedLoad   :  on (all topologies)"
printf "║  %-70s║\n" "ML policy   :  explore=off  learn=off  resume=on"
echo "╚══════════════════════════════════════════════════════════════════════╝"
echo

# ---- per-topology loop -------------------------------------------------------
for topo in "${TOPOS[@]}"; do

  crip_flag=(--no-cripple)
  crip_label="off"
  if topo_supports_cripple "$topo"; then
    crip_flag=(--cripple)
    crip_label="on"
  fi

  echo "┌──────────────────────────────────────────────────────────────────────┐"
  printf "│  Topology: %-10s   priority: %-10s   cripple: %-14s│\n" \
         "$topo" "$PRIORITY" "$crip_label"
  echo "│  Launching ML sweep and baseline sweep in parallel...                │"
  echo "└──────────────────────────────────────────────────────────────────────┘"

  # -- ML sweep (background) --------------------------------------------------
  # Each of the $WORKERS seeds gets its own Python service slot, same as
  # training. Seeds run in parallel; services are cleaned up by run_tests.sh's
  # EXIT trap when this subshell finishes.
  "$RUN_TESTS" seeds \
    --topology    "$topo"         \
    --seeds       "$N_SEEDS"      \
    --seed        "$BASE_SEED"    \
    --simTime     "$SIM_TIME"     \
    --priority    "$PRIORITY"     \
    --trafficMode "$TRAFFIC_MODE" \
    --workers     "$WORKERS"      \
    --ml                          \
    --no-explore                  \
    --no-learn                    \
    --failures                    \
    --mixedLoad                   \
    "${crip_flag[@]}"             \
    2>&1 | sed "s/^/[ml:$topo] /" &
  ml_pid=$!

  # -- Baseline sweep (background) --------------------------------------------
  # No ML service needed; $WORKERS ns-3 processes run in parallel.
  "$RUN_TESTS" seeds \
    --topology    "$topo"         \
    --seeds       "$N_SEEDS"      \
    --seed        "$BASE_SEED"    \
    --simTime     "$SIM_TIME"     \
    --trafficMode "$TRAFFIC_MODE" \
    --workers     "$WORKERS"      \
    --no-ml                       \
    --failures                    \
    --mixedLoad                   \
    "${crip_flag[@]}"             \
    2>&1 | sed "s/^/[base:$topo] /" &
  base_pid=$!

  # -- Wait for both sweeps to finish before moving to the next topology ------
  ml_ok=0;   wait "$ml_pid"   || ml_ok=$?
  base_ok=0; wait "$base_pid" || base_ok=$?

  if (( ml_ok != 0 ));   then echo "  [WARN] ML sweep exited with code $ml_ok";   fi
  if (( base_ok != 0 )); then echo "  [WARN] Baseline sweep exited with code $base_ok"; fi
  echo

done

# ---- footer ------------------------------------------------------------------
echo "╔══════════════════════════════════════════════════════════════════════╗"
printf "║  %-70s║\n" "BENCHMARK COMPLETE"
printf "║  %-70s║\n" ""
printf "║  %-70s║\n" "Logs    :  scratch/data/results/logs/"
printf "║  %-70s║\n" "Summary :  scratch/data/results/summary.csv"
echo "╚══════════════════════════════════════════════════════════════════════╝"
