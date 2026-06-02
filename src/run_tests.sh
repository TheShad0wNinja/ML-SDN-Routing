#!/usr/bin/env bash
# scratch/run_tests.sh — drive ns-3 SDN test variants.
#
# Modes (every run is teed to scratch/data/results/logs/ and summarized into
# scratch/data/results/summary.csv):
#   single     one run with whatever flags you pass
#   compare    baseline + one ML run, same seed/params
#   presets    baseline + 3 ML priority variants
#   seeds      repeat one config across N consecutive seeds (in parallel)
#   matrix     seeds × {central,random,grouped} × {failures} × {ml,baseline}
#   eval       pretrained eval-only run (--mlExplore=false --mlLearn=false --mlResume=true)
#   train      federated training. --sections N --workers M --rounds R.
#              Spawns N*M ns-3 processes total (M replicas per section),
#              FedAvg'd by root_aggregator.py via scratch/data/federated_weights/.
#              sections=1 (default) → workers all run the full topology.
#              sections>1 → must match scenario's section count; workers per section.
#   summary    parse a saved log and print headline numbers
#   clean      wipe checkpoints + replay buffer

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NS3_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RESULTS_DIR="$SCRIPT_DIR/data/results"
LOG_DIR="$RESULTS_DIR/logs"
SUMMARY_CSV="$RESULTS_DIR/summary.csv"
ML_SERVICE_PY="$SCRIPT_DIR/python/controller/ml_service.py"
# Agent checkpoints are stored per-priority so balanced / throughput / energy
# / custom can be trained independently and toggled at run time via --priority.
# Layout: scratch/data/agent/<priority>/        single-worker
#         scratch/data/agent/<priority>/w<N>/   multi-worker (train mode)
#         scratch/data/federated_weights/<priority>/   FedAvg exchange
CKPT_ROOT="$SCRIPT_DIR/data/agent"
ckpt_dir_for_priority() { echo "$CKPT_ROOT/$1"; }

mkdir -p "$LOG_DIR"

# ----- defaults --------------------------------------------------------------
# --topology, --sections, --workers have no defaults: they must be passed
# explicitly on every invocation that needs them (train always; others on
# demand). This prevents silently running on the wrong topology or training
# with a stale parallelism setting.
TOPOLOGY=""
SECTIONS=""
WORKERS=""
MIX=""                    # train mode: "topoA:N,topoB,topoC:N" heterogeneous pool

SIM_TIME=600
WARMUP=10
TRAFFIC_MODE="central"
SEED=12345
PRIORITY="balanced"
ML=false
MIXED_LOAD=true
FAILURES=false
CRIPPLE=false
MULTI_CONTROLLER=false
EXPLORE=true
LEARN=true
RESUME=true
NOISE_SIGMA=""            # --noiseSigma: pin Python agent's exploration sigma
N_SEEDS=5
EVAL_WINDOW=0
EXTRA_ARGS=()
AUTO_ML=true
NS3_VERBOSE=false
ML_PORT_BASE=5555
RUN_ID=""      # unique per-invocation token for deploy dir isolation

# Agent directory layout. Switched by mode (not by WORKERS) so training scratch
# never collides with deployment runtime.
#   flat   — single dir at $base (single-controller modes; the canonical model)
#   train  — $base/train/w<id>/  (per-worker training scratch; FedAvg producer)
#   deploy — $base/deploy/s<id>/ (per-section deployment runtime; seeded from
#            $base/local.pt before each multiController run)
ML_LAYOUT="flat"

# Federated training (used by `train` mode).
FED_ROUNDS=1              # back-to-back rounds in one invocation
FED_RESET=false           # --reset: wipe priority's training state before training
FEDAVG_DIR=""
FEDAVG_EVERY=50
FEDAVG_TIMEOUT=300
FEDAVG_AGG_LOG_DIR=""
AGG_PID=""
WE_STARTED_AGG=false

ML_PID=""
WE_STARTED_ML=false
declare -a WORKER_ML_PIDS=()
declare -a WORKER_AGENT_DIRS=()

# ----- usage -----------------------------------------------------------------
usage() {
  cat <<'EOF'
Usage: scratch/run_tests.sh <mode> [options] [-- extra-ns3-args]

Modes:
  single                One run with the given flags. Pass --multiController
                        for in-process M-controller inference using trained
                        FedAvg weights (auto-enables --ml --mlResume).
  compare               Baseline + ML run, same seed/params
  presets               Baseline + 3 ML priority presets (balanced/delay/energy)
  seeds                 Sweep N consecutive seeds for one config
  matrix                Full matrix: seeds × {central,random,grouped} × {failures} × {ml,baseline}
  eval                  ML run with --mlExplore=false --mlLearn=false --mlResume=true
  train                 Federated training. --sections N --workers M --rounds R
                        launches N*M parallel ns-3 processes (M replicas per
                        section), federated via scratch/data/federated_weights/.
                        sections=1 (default) → workers all run the full topology
                        with different seeds; sections>1 must match the
                        scenario's section count.
  summary FILE          Print headline stats from a saved log file
  clean                 Remove ML checkpoint + replay buffer for --priority
                        (only that priority — others are untouched)

Options:
  --topology X          usa | fat-tree-k4 | two-switch-ping | sensor-cluster | geant
                        Note: fat-tree-k4 has 1 section; --multiController
                        and --sections>1 are unsupported there.
  --simTime N           Simulation duration (s)                    (default: 600)
  --warmupS N           Warmup window                              (default: 10)
  --trafficMode X       random | central | grouped                 (default: central)
  --seed N              Base random seed                           (default: 12345)
  --seeds N             Number of seeds for seeds/matrix modes     (default: 5)
  --priority X          balanced | throughput | energy | custom (default: balanced)
                        Each priority has its own dir under
                        scratch/data/agent/<priority>/ — train independently
                        and switch via this flag. Layout:
                          local.pt          canonical model (FedAvg result)
                          train/w<id>/      per-worker training scratch
                          deploy/s<id>/     per-section multiController runtime
                                            (re-seeded from local.pt each run)
  --ml | --no-ml        Enable / disable the ML controller         (default: off)
  --mixedLoad | --no-mixedLoad
                        Toggle mixed-protocol (TCP+UDP) background load
                                                                   (default: on)
  --failures | --no-failures   Toggle scheduled link churn         (default: off)
  --cripple  | --no-cripple    Toggle Missoula crippling (USA)     (default: off)
  --multiController     Run M in-process controllers using trained weights
  --no-explore          Disable Gaussian action noise (hard off; sigma ignored)
  --no-learn            Disable gradient updates / train_step (frozen policy)
  --no-resume           Don't resume the ML agent from checkpoint
  --noiseSigma X        Pin the Python agent's exploration noise sigma to X,
                        overriding both the default 0.3 init and the value
                        saved in the checkpoint. Use for a low-noise cooldown
                        / robust-exploit fine-tune phase on top of already-
                        trained weights (e.g. --noiseSigma 0.05 for 3 rounds).
                        Unset / negative = use the default decay schedule.
  --evalWindowS N       Delay FlowMonitor reset by N seconds past warmup
  --no-auto-ml          Don't auto-start the Python ML service
  --verbose             Pass NS_LOG to surface controller info
  --workers N           Parallel ns-3 processes. In `train` mode this is
                        per-section, so total = sections * workers. Each worker
                        gets its own port (5555+w), checkpoint dir, and seed.
                        REQUIRED for `train`; defaults to 1 for other modes.

Train-mode flags:
  --sections N              Partition count. N=1 = full topology (no partition).
                            N>1 must match the scenario's declared section count.
                            REQUIRED for `train`; defaults to 1 for other modes.
  --rounds N                Chain N back-to-back rounds in one invocation
                            (default 1). Local checkpoints + aggregator state
                            persist across rounds AND across invocations:
                            running `train` again resumes from the latest
                            global_round_*.pt under the priority's FedAvg
                            dir. New worker slots (if --workers grew) are
                            bootstrapped from the latest global.
  --reset                   Wipe the priority's FedAvg dir + per-worker
                            checkpoints before training. Use when you
                            actually want to start from scratch.
  --fedAvgEverySteps K      Workers submit weights every K training steps,
                            then block on the global model (default: 0 = off).
  --fedAvgTimeoutS S        Per-round timeout (default: 300).

  -- arg1 arg2 ...      Forward extra args verbatim to ns3 (after --)

Where logs go:   scratch/data/results/logs/
Summary CSV:     scratch/data/results/summary.csv
EOF
}

# ----- ML service lifecycle --------------------------------------------------
ml_port_for_slot()      { echo $((ML_PORT_BASE + $1)); }
ml_endpoint_for_slot()  { echo "tcp://127.0.0.1:$(ml_port_for_slot "$1")"; }
ml_agent_dir_for_slot() {
  local base; base=$(ckpt_dir_for_priority "$PRIORITY")
  case "$ML_LAYOUT" in
    train)      echo "$base/train/w$1" ;;
    deploy)     echo "$base/deploy/$RUN_ID/s$1" ;;
    flat|*)     echo "$base" ;;
  esac
}

# Generate a unique ID for this invocation (idempotent: no-op if already set).
init_run_id() {
  [[ -n "$RUN_ID" ]] && return
  local rand
  rand=$(tr -dc a-f0-9 </dev/urandom 2>/dev/null | head -c6 \
         || printf '%06x' $(( (RANDOM * 65536 + RANDOM) & 0xFFFFFF )))
  RUN_ID="$(date +%Y%m%d-%H%M%S)-${rand}"
}

# Find N consecutive TCP ports that are not currently bound, in 5560-9400.
# Prints the base port; callers use base..base+N-1.
pick_free_port_base() {
  local n="${1:-1}"
  local stride=$(( n < 1 ? 1 : n ))
  local used
  used=$(ss -ltn 2>/dev/null \
         | awk 'NR>1 {split($4,a,":"); print a[length(a)]}' \
         | sort -n)
  local slots=$(( (9400 - 5560) / stride ))
  local i base p ok
  for ((i=0; i<40; i++)); do
    base=$(( 5560 + (RANDOM % slots) * stride ))
    ok=true
    for ((p=base; p<base+stride; p++)); do
      echo "$used" | grep -q "^${p}$" && { ok=false; break; }
    done
    $ok && { echo "$base"; return 0; }
  done
  echo "5555"  # last-resort fallback
}

# For every non-train ML run: redirect runtime artifacts (local.pt, metrics.csv,
# replay.pkl) to $base/deploy/$RUN_ID/s<slot>/ — a unique per-invocation dir —
# so concurrent eval/single runs never share files, ports, or metrics. The
# canonical local.pt at the priority root is never overwritten. The deploy/
# subtree can be deleted freely; the next run re-seeds from $base/local.pt.
# No-op when ML=false, when already in deploy layout (multiController path),
# or in train layout (training manages its own dirs).
ensure_deploy_layout() {
  $ML || return 0
  [[ "$ML_LAYOUT" == "deploy" || "$ML_LAYOUT" == "train" ]] && return 0
  ML_LAYOUT="deploy"
  init_run_id
  ML_PORT_BASE=$(pick_free_port_base "$WORKERS")
  seed_deploy_dirs
}

# Copy the canonical $base/local.pt into each deploy section dir so every
# controller in a --multiController run loads the same FedAvg'd weights.
# Called from cmd_single before start_ml_service when ML_LAYOUT=deploy,
# and by ensure_deploy_layout for all other non-train ML runs.
seed_deploy_dirs() {
  local base; base=$(ckpt_dir_for_priority "$PRIORITY")
  local src="$base/local.pt"
  if [[ ! -f "$src" ]]; then
    echo "[run_tests] WARN: no $src — deploy sections will start from random init." >&2
    echo "[run_tests]       Train first (run_tests.sh train …) or provide weights." >&2
    return 0
  fi
  local s
  for ((s=0; s<WORKERS; s++)); do
    local d; d=$(ml_agent_dir_for_slot "$s")
    mkdir -p "$d"
    cp "$src" "$d/local.pt"
    rm -f "$d/replay.pkl"            # stale replay would confound a fresh deploy
  done
  echo "[run_tests] Seeded $WORKERS deploy section(s) from $src"
}

# True only when the python service is actually pumping its REP loop. A
# TCP-connect probe returns true the instant bind() runs, but service.run()
# may still be inside the torch import; sending a real ZMQ request forces us
# to wait until the dispatch loop is alive.
ml_port_listening() {
  local port="$1"
  python3 - "$port" <<'PY' 2>/dev/null
import sys
try:
    import zmq
except ImportError:
    import socket
    port = int(sys.argv[1])
    s = socket.socket(); s.settimeout(0.4)
    try: s.connect(("127.0.0.1", port)); s.close(); sys.exit(0)
    except OSError: sys.exit(1)
port = int(sys.argv[1])
ctx = zmq.Context()
sock = ctx.socket(zmq.REQ)
sock.setsockopt(zmq.RCVTIMEO, 800)
sock.setsockopt(zmq.SNDTIMEO, 800)
sock.setsockopt(zmq.LINGER, 0)
try:
    sock.connect(f"tcp://127.0.0.1:{port}")
    sock.send(b'{"cmd":"get_action"}')
    sock.recv()
    sys.exit(0)
except Exception:
    sys.exit(1)
finally:
    sock.close(0); ctx.term()
PY
}

start_ml_service() {
  $AUTO_ML || return 0
  WORKER_ML_PIDS=()
  WORKER_AGENT_DIRS=()
  local s
  for ((s=0; s<WORKERS; s++)); do
    local port; port=$(ml_port_for_slot "$s")
    local dir;  dir=$(ml_agent_dir_for_slot "$s")
    mkdir -p "$dir"
    WORKER_AGENT_DIRS[$s]="$dir"

    if ml_port_listening "$port"; then
      if [[ "$ML_LAYOUT" == "train" ]]; then
        # In train mode a stale service (e.g. leftover deploy/eval run) would
        # have the wrong fedavg dir and worker_id — kill it and start fresh.
        local stale_pid
        stale_pid=$(lsof -ti tcp:"$port" 2>/dev/null | head -1 || true)
        if [[ -n "$stale_pid" ]]; then
          echo "[run_tests] Train mode: killing stale ML service on :$port (pid $stale_pid)."
          kill -TERM "$stale_pid" 2>/dev/null || true
          sleep 1
        fi
      else
        echo "[run_tests] ML service already on :$port — reusing (slot $s, dir $dir)."
        WORKER_ML_PIDS[$s]=""
        continue
      fi
    fi

    # Rotate any stale metrics.csv from a previous run so summarize_log
    # only sees this run's rows. Skip when reusing a live service above —
    # that process still owns its file handle.
    if [[ -f "$dir/metrics.csv" ]]; then
      mv -f "$dir/metrics.csv" "$dir/metrics.prev.csv"
    fi

    echo "[run_tests] Starting ML service slot=$s port=$port dir=$dir…"
    (
      cd "$NS3_ROOT"
      NS3_ML_PORT="$port" \
        NS3_ML_AGENT_DIR="$dir" \
        NS3_WORKER_ID="$s" \
        NS3_FEDAVG_DIR="${FEDAVG_DIR:-}" \
        NS3_FEDAVG_EVERY_STEPS="${FEDAVG_EVERY:-0}" \
        NS3_FEDAVG_WAIT_TIMEOUT_S="${FEDAVG_TIMEOUT:-300}" \
        exec python3 -u "$ML_SERVICE_PY"
    ) >"$LOG_DIR/ml-service-w$s.log" 2>&1 &
    WORKER_ML_PIDS[$s]=$!
    WE_STARTED_ML=true
  done

  # Torch import is the slow part on cold start (~10s); allow 30s.
  local elapsed=0 max=30 all_ready=false
  while (( elapsed < max )); do
    all_ready=true
    for ((s=0; s<WORKERS; s++)); do
      local pid="${WORKER_ML_PIDS[$s]:-}"
      [[ -z "$pid" ]] && continue
      if ! kill -0 "$pid" 2>/dev/null; then
        echo "[run_tests] ERROR: ML slot $s died during startup. Last log lines:" >&2
        tail -20 "$LOG_DIR/ml-service-w$s.log" >&2 || true
        return 1
      fi
      ml_port_listening "$(ml_port_for_slot "$s")" || all_ready=false
    done
    $all_ready && { echo "[run_tests] All $WORKERS ML services ready (~${elapsed}s)."; return 0; }
    sleep 1; elapsed=$((elapsed + 1))
  done
  echo "[run_tests] WARN: not all ML services bound within ${max}s." >&2
  return 1
}

stop_ml_service() {
  $WE_STARTED_ML || return 0
  for ((s=0; s<${#WORKER_ML_PIDS[@]}; s++)); do
    local pid="${WORKER_ML_PIDS[$s]:-}"
    [[ -z "$pid" ]] && continue
    echo "[run_tests] Stopping ML slot=$s (pid $pid)…"
    kill -TERM "$pid" 2>/dev/null || true
  done
  # Give each service up to 10s to finish saving its checkpoint.
  local waited=0
  while (( waited < 20 )); do
    local any_alive=false
    for pid in "${WORKER_ML_PIDS[@]}"; do
      [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null && { any_alive=true; break; }
    done
    $any_alive || break
    sleep 0.5; waited=$((waited + 1))
  done
  for pid in "${WORKER_ML_PIDS[@]}"; do
    [[ -n "$pid" ]] || continue
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
  WORKER_ML_PIDS=()
  WE_STARTED_ML=false
}

start_aggregator() {
  $WE_STARTED_AGG && return 0
  [[ -n "$FEDAVG_DIR" ]] || return 0
  mkdir -p "$FEDAVG_DIR" "$FEDAVG_AGG_LOG_DIR"
  echo "[run_tests] Starting root aggregator (workers=$WORKERS dir=$FEDAVG_DIR every=$FEDAVG_EVERY)…"
  (
    cd "$NS3_ROOT"
    exec python3 -u "$SCRIPT_DIR/python/controller/root_aggregator.py" \
      --dir "$FEDAVG_DIR" \
      --num-workers "$WORKERS" \
      --arch-tag gnn-v6 \
      --round-timeout-s "$FEDAVG_TIMEOUT" \
      --log-dir "$FEDAVG_AGG_LOG_DIR"
  ) >"$LOG_DIR/root-aggregator.log" 2>&1 &
  AGG_PID=$!
  WE_STARTED_AGG=true
  sleep 1
  if ! kill -0 "$AGG_PID" 2>/dev/null; then
    echo "[run_tests] ERROR: root aggregator died at startup. Last lines:" >&2
    tail -20 "$LOG_DIR/root-aggregator.log" >&2 || true
    return 1
  fi
}

stop_aggregator() {
  $WE_STARTED_AGG || return 0
  [[ -z "$AGG_PID" ]] && return 0
  echo "[run_tests] Stopping root aggregator (pid $AGG_PID)…"
  kill -TERM "$AGG_PID" 2>/dev/null || true
  wait "$AGG_PID" 2>/dev/null || true
  AGG_PID=""
  WE_STARTED_AGG=false
}

trap 'stop_ml_service; stop_aggregator' EXIT INT TERM

# ----- topology capability registry -----------------------------------------
# Single source of truth for what each topology supports. Edit ONE block to
# add a new topology; topology_supports_flag, topology_section_count, and
# the train-mode curriculum all read from here.
#
# Keys:
#   sections      — int, # of sections defined in the C++ topology spec
#   supports_mixed_load — 1/0  (mixed-protocol TCP+UDP background traffic)
#   supports_fail       — 1/0
#   supports_crip       — 1/0  (only USA has the Missoula cripple)
#   modes         — space-separated trafficMode values valid for this topo
topology_cap() {
  local topo="$1" key="$2"
  case "$topo:$key" in
    usa:sections)                  echo 3 ;;
    usa:supports_mixed_load)       echo 1 ;;
    usa:supports_fail)             echo 1 ;;
    usa:supports_crip)             echo 1 ;;
    usa:modes)                     echo "central random grouped" ;;
    fat-tree-k4:sections)          echo 1 ;;
    fat-tree-k4:supports_mixed_load) echo 1 ;;
    fat-tree-k4:supports_fail)     echo 1 ;;
    fat-tree-k4:supports_crip)     echo 0 ;;
    fat-tree-k4:modes)             echo "central random" ;;
    sensor-cluster:sections)          echo 1 ;;
    sensor-cluster:supports_mixed_load) echo 1 ;;
    sensor-cluster:supports_fail)     echo 1 ;;
    sensor-cluster:supports_crip)     echo 0 ;;
    sensor-cluster:modes)             echo "central random" ;;
    geant:sections)            echo 1 ;;
    geant:supports_mixed_load) echo 1 ;;
    geant:supports_fail)       echo 1 ;;
    geant:supports_crip)       echo 0 ;;
    geant:modes)               echo "central random grouped" ;;
    two-switch-ping:sections)      echo 1 ;;
    two-switch-ping:supports_mixed_load) echo 0 ;;
    two-switch-ping:supports_fail) echo 0 ;;
    two-switch-ping:supports_crip) echo 0 ;;
    two-switch-ping:modes)         echo "central" ;;
    *) echo "" ;;
  esac
}

topology_supports_flag() {
  local topo="$1" flag="$2"
  case "$flag" in
    cripple)  [[ "$(topology_cap "$topo" supports_crip)" == "1" ]] ;;
    failures) [[ "$(topology_cap "$topo" supports_fail)" == "1" ]] ;;
    mixedLoad) [[ "$(topology_cap "$topo" supports_mixed_load)" == "1" ]] ;;
    simTime|warmupS|seed|trafficMode|backboneQueue|edgeQueue|evalWindowOffsetS)
      return 0 ;;
    ml|mlPriority|mlExplore|mlResume|mlEndpoint|mlPortBase|mlNoiseSigma|multiController|sections|sectionId)
      # ML/multi-ctrl knobs are defined by the runner and accepted everywhere
      # except the bare-bones two-switch-ping sanity binary.
      [[ "$topo" != "two-switch-ping" ]] ;;
    *) return 0 ;;
  esac
}

# Variants for the train-mode round-robin curriculum. Emits one line per
# variant in "mode|failures|cripple" form (failures/cripple are 1/0).
topology_variants() {
  local topo="$1"
  local modes; modes=$(topology_cap "$topo" modes)
  local fails="0"; [[ "$(topology_cap "$topo" supports_fail)" == "1" ]] && fails="0 1"
  local crips="0"; [[ "$(topology_cap "$topo" supports_crip)" == "1" ]] && crips="0 1"
  local m f c
  for m in $modes; do
    for f in $fails; do
      for c in $crips; do
        echo "${m}|${f}|${c}"
      done
    done
  done
}

add_if_supported() {
  local flag="$1"
  if topology_supports_flag "$TOPOLOGY" "$flag"; then
    NS3_ARGS+=("--${flag}=$2")
  fi
}

add_bool_if_supported() {
  local flag="$1" enabled="$2"
  topology_supports_flag "$TOPOLOGY" "$flag" || return 0
  if [[ "$enabled" == "true" ]]; then
    NS3_ARGS+=("--$flag")
  fi
}

build_args() {
  NS3_ARGS=()
  add_if_supported simTime     "$SIM_TIME"
  add_if_supported warmupS     "$WARMUP"
  add_if_supported trafficMode "$TRAFFIC_MODE"
  add_if_supported seed        "$SEED"
  add_bool_if_supported mixedLoad "$MIXED_LOAD"
  add_bool_if_supported failures "$FAILURES"
  add_bool_if_supported cripple  "$CRIPPLE"
  add_bool_if_supported multiController "$MULTI_CONTROLLER"
  if $ML; then
    add_bool_if_supported ml true
    add_if_supported mlPriority "$PRIORITY"
    if [[ "$EXPLORE" == "false" ]]; then add_if_supported mlExplore false; fi
    if [[ "$LEARN"   == "false" ]]; then add_if_supported mlLearn   false; fi
    if [[ "$RESUME"  == "false" ]]; then add_if_supported mlResume  false; fi
    if [[ -n "$NOISE_SIGMA" ]]; then add_if_supported mlNoiseSigma "$NOISE_SIGMA"; fi
    if (( EVAL_WINDOW > 0 )); then add_if_supported evalWindowOffsetS "$EVAL_WINDOW"; fi
  fi
  NS3_ARGS+=("${EXTRA_ARGS[@]}")
}

# ----- log parsing -----------------------------------------------------------
extract() {
  local pat="$1" file="$2"
  grep -E "$pat" "$file" 2>/dev/null | head -n1 | grep -oE '[0-9]+(\.[0-9]+)?' | head -n1 || true
}

fmt_J()  { awk -v v="${1:-0}" 'BEGIN{printf (v>=1e6)?"%.2f MJ":"%.0f J", (v>=1e6)?v/1e6:v}'; }
fmt_W()  { awk -v v="${1:-0}" 'BEGIN{printf (v>=1000)?"%.2f kW":"%.0f W", (v>=1000)?v/1000:v}'; }

# Extract a single field from the Per-Class FlowMonitor table.
# fld: 2=flows 3=tx 4=rx 5=loss_pct 6=delay_ms 7=p99_ms 8=mbps
extract_class() {
  local cls="$1" fld="$2" file="$3"
  awk -v c="$cls" -v f="$fld" '
    /=== Per-Class FlowMonitor ===/ { inb=1; next }
    inb && /^=== /                  { inb=0 }
    inb && NF>=8 && $1==c           { print $f; exit }
  ' "$file"
}

summarize_log() {
  local f="$1" label="$2" metrics_override="${3:-}"
  local success rtt jitter delivery delay flows tx rx hops
  success=$(extract  '^[[:space:]]+Success[[:space:]]+:' "$f")
  rtt=$(extract      '^[[:space:]]+Avg RTT[[:space:]]+:' "$f")
  jitter=$(extract   '^[[:space:]]+Avg jitter[[:space:]]+:' "$f")
  delivery=$(extract '^[[:space:]]+Delivery[[:space:]]+:' "$f")
  delay=$(extract    '^[[:space:]]+Avg delay[[:space:]]+:' "$f")
  flows=$(extract    '^[[:space:]]+Flows[[:space:]]+:' "$f")
  tx=$(extract       '^[[:space:]]+Tx packets[[:space:]]+:' "$f")
  rx=$(extract       '^[[:space:]]+Rx packets[[:space:]]+:' "$f")
  hops=$(extract     '^[[:space:]]+Avg hops[[:space:]]+:' "$f")
  local energy residual power per_sw_consumed per_sw_residual residual_frac
  energy=$(extract  '^[[:space:]]+Total consumed[[:space:]]+:' "$f")
  residual=$(extract '^[[:space:]]+Total residual[[:space:]]+:' "$f")
  power=$(extract   '^[[:space:]]+Total avg power[[:space:]]+:' "$f")
  per_sw_consumed=$(extract '^[[:space:]]+Per-switch consumed[[:space:]]+:' "$f")
  per_sw_residual=$(extract '^[[:space:]]+Per-switch residual[[:space:]]+:' "$f")
  residual_frac=$(extract   '^[[:space:]]+Residual fraction[[:space:]]+:' "$f")

  local lost_pkts
  lost_pkts=$(extract '^[[:space:]]+Lost packets[[:space:]]*:' "$f")

  local net_lifetime_s first_death_dpid min_residual_pct min_residual_dpid max_residual_pct max_residual_dpid
  net_lifetime_s=$(extract   '^[[:space:]]+Network lifetime[[:space:]]*:'  "$f")
  first_death_dpid=$(extract '^[[:space:]]+First death dpid[[:space:]]*:' "$f")
  min_residual_pct=$(extract '^[[:space:]]+Min residual pct[[:space:]]*:' "$f")
  min_residual_dpid=$(extract '^[[:space:]]+Min residual dpid[[:space:]]*:' "$f")
  max_residual_pct=$(extract '^[[:space:]]+Max residual pct[[:space:]]*:' "$f")
  max_residual_dpid=$(extract '^[[:space:]]+Max residual dpid[[:space:]]*:' "$f")

  local bulk_flows bulk_tx bulk_rx bulk_loss bulk_delay bulk_p99 bulk_mbps
  local iot_flows  iot_tx  iot_rx  iot_loss  iot_delay  iot_p99  iot_mbps
  local video_flows video_tx video_rx video_loss video_delay video_p99 video_mbps
  local voip_flows voip_tx  voip_rx  voip_loss  voip_delay  voip_p99  voip_mbps
  local web_flows  web_tx   web_rx   web_loss   web_delay   web_p99   web_mbps
  for cls in bulk iot video voip web; do
    eval "${cls}_flows=\$(extract_class \"$cls\" 2 \"\$f\")"
    eval "${cls}_tx=\$(extract_class    \"$cls\" 3 \"\$f\")"
    eval "${cls}_rx=\$(extract_class    \"$cls\" 4 \"\$f\")"
    eval "${cls}_loss=\$(extract_class  \"$cls\" 5 \"\$f\")"
    eval "${cls}_delay=\$(extract_class \"$cls\" 6 \"\$f\")"
    eval "${cls}_p99=\$(extract_class   \"$cls\" 7 \"\$f\")"
    eval "${cls}_mbps=\$(extract_class  \"$cls\" 8 \"\$f\")"
  done

  local jpermb=""
  if [[ -n "$energy" && -n "$rx" && "$rx" != "0" ]]; then
    # Assume ~1KB avg payload — same approximation across runs makes this
    # comparable; absolute number is not the point.
    jpermb=$(awk -v e="$energy" -v r="$rx" 'BEGIN{printf "%.3f", e/(r*1024*8/1e6)}')
  fi

  local ml_ticks ml_reward_final ml_reward_mean ml_critic_loss ml_actor_loss
  ml_ticks=""; ml_reward_final=""; ml_reward_mean=""
  ml_critic_loss=""; ml_actor_loss=""
  # Where the aggregator reads metrics from. A caller (run_one in train mode)
  # may pass this worker's own agent dir so the row reflects ONLY that worker —
  # otherwise every per-worker summarize call would pool all workers' metrics.csv
  # under the priority dir and write the same blurred aggregate into every row.
  # With no override we hand over the whole priority dir, which auto-discovers a
  # top-level metrics.csv (single-worker / eval) or pools deploy/s* sections of a
  # single --multiController run (where pooling is the intended whole-deployment view).
  local metrics_src
  if [[ -n "$metrics_override" ]]; then
    metrics_src="$metrics_override"
  else
    metrics_src="$(ckpt_dir_for_priority "$PRIORITY")"
  fi
  if $ML && [[ -d "$metrics_src" ]]; then
    local agg
    agg=$(python3 "$SCRIPT_DIR/python/aggregate_metrics.py" \
            "$metrics_src" 2>/dev/null || true)
    ml_ticks=$(echo       "$agg" | awk -F= '/^ML_TICKS=/{print $2}')
    ml_reward_final=$(echo "$agg" | awk -F= '/^ML_REWARD_FINAL=/{print $2}')
    ml_reward_mean=$(echo  "$agg" | awk -F= '/^ML_REWARD_MEAN_LAST25=/{print $2}')
    ml_critic_loss=$(echo  "$agg" | awk -F= '/^ML_CRITIC_LOSS_FINAL=/{print $2}')
    ml_actor_loss=$(echo   "$agg" | awk -F= '/^ML_ACTOR_LOSS_FINAL=/{print $2}')
  fi

  echo "── $label ──"
  printf "  ping success : %s%%\n"    "${success:-—}"
  printf "  avg RTT      : %s ms\n"   "${rtt:-—}"
  printf "  jitter       : %s ms\n"   "${jitter:-—}"
  printf "  delivery     : %s%%\n"    "${delivery:-—}"
  printf "  avg delay    : %s ms\n"   "${delay:-—}"
  printf "  avg hops     : %s\n"      "${hops:-—}"
  printf "  total energy : %s  (avg %s)\n" "$(fmt_J "${energy:-0}")" "$(fmt_W "${power:-0}")"
  [[ -n "$per_sw_consumed" ]] && printf "  per-switch   : consumed %s, left %s (%s%%)\n" \
    "$(fmt_J "${per_sw_consumed:-0}")" "$(fmt_J "${per_sw_residual:-0}")" "${residual_frac:-—}"
  [[ -n "$jpermb" ]] && printf "  J / Mb delivered : %s\n" "$jpermb"
  [[ -n "$ml_reward_final" ]] && printf "  ML reward    : final=%s  mean_last25=%s  ticks=%s\n" \
    "$ml_reward_final" "${ml_reward_mean:-—}" "${ml_ticks:-0}"
  echo

  # CSV: rotate aside if header changed (old flat schema, partial archive).
  local expected_header="timestamp,label,topology,sim_time_s,traffic_mode,seed,controller,priority,mixed_load,failures,cripple,ping_success_pct,rtt_avg_ms,rtt_jitter_ms,pdr_pct,e2e_delay_avg_ms,flows,tx_pkts,rx_pkts,hop_count_avg,energy_total_j,energy_residual_j,power_avg_w,per_sw_consumed_j,per_sw_residual_j,residual_pct,j_per_mb,ml_reward_final,ml_reward_mean_last25,ml_critic_loss_final,ml_actor_loss_final,ml_ticks,lost_pkts,bulk_flows,bulk_tx,bulk_rx,bulk_loss_pct,bulk_delay_ms,bulk_p99_ms,bulk_mbps,iot_flows,iot_tx,iot_rx,iot_loss_pct,iot_delay_ms,iot_p99_ms,iot_mbps,video_flows,video_tx,video_rx,video_loss_pct,video_delay_ms,video_p99_ms,video_mbps,voip_flows,voip_tx,voip_rx,voip_loss_pct,voip_delay_ms,voip_p99_ms,voip_mbps,web_flows,web_tx,web_rx,web_loss_pct,web_delay_ms,web_p99_ms,web_mbps,net_lifetime_s,first_death_dpid,min_residual_pct,min_residual_dpid,max_residual_pct,max_residual_dpid"
  if [[ -f "$SUMMARY_CSV" ]] && [[ "$(head -n1 "$SUMMARY_CSV")" != "$expected_header" ]]; then
    local archived="${SUMMARY_CSV%.csv}.$(date +%Y%m%d-%H%M%S).csv"
    mv "$SUMMARY_CSV" "$archived"
    echo "[run_tests] CSV schema changed — archived old rows to $archived"
  fi
  if [[ ! -f "$SUMMARY_CSV" ]]; then
    echo "$expected_header" >"$SUMMARY_CSV"
  fi
  printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
    "$(date -Iseconds)" "$label" "$TOPOLOGY" "$SIM_TIME" "$TRAFFIC_MODE" "$SEED" \
    "$($ML && echo ml || echo baseline)" "$($ML && echo "$PRIORITY" || echo -)" \
    "$($MIXED_LOAD && echo 1 || echo 0)" "$($FAILURES && echo 1 || echo 0)" "$($CRIPPLE && echo 1 || echo 0)" \
    "${success:-}" "${rtt:-}" "${jitter:-}" "${delivery:-}" "${delay:-}" \
    "${flows:-}" "${tx:-}" "${rx:-}" "${hops:-}" \
    "${energy:-}" "${residual:-}" "${power:-}" \
    "${per_sw_consumed:-}" "${per_sw_residual:-}" "${residual_frac:-}" "${jpermb:-}" \
    "${ml_reward_final:-}" "${ml_reward_mean:-}" "${ml_critic_loss:-}" "${ml_actor_loss:-}" \
    "${ml_ticks:-}" \
    "${lost_pkts:-}" \
    "${bulk_flows:-}"  "${bulk_tx:-}"  "${bulk_rx:-}"  "${bulk_loss:-}"  "${bulk_delay:-}"  "${bulk_p99:-}"  "${bulk_mbps:-}" \
    "${iot_flows:-}"   "${iot_tx:-}"   "${iot_rx:-}"   "${iot_loss:-}"   "${iot_delay:-}"   "${iot_p99:-}"   "${iot_mbps:-}" \
    "${video_flows:-}" "${video_tx:-}" "${video_rx:-}" "${video_loss:-}" "${video_delay:-}" "${video_p99:-}" "${video_mbps:-}" \
    "${voip_flows:-}"  "${voip_tx:-}"  "${voip_rx:-}"  "${voip_loss:-}"  "${voip_delay:-}"  "${voip_p99:-}"  "${voip_mbps:-}" \
    "${web_flows:-}"   "${web_tx:-}"   "${web_rx:-}"   "${web_loss:-}"   "${web_delay:-}"   "${web_p99:-}"   "${web_mbps:-}" \
    "${net_lifetime_s:-}" "${first_death_dpid:-}" \
    "${min_residual_pct:-}" "${min_residual_dpid:-}" \
    "${max_residual_pct:-}" "${max_residual_dpid:-}" \
    >>"$SUMMARY_CSV"
}

# ----- runner ----------------------------------------------------------------
# Build once up front. Concurrent ./ns3 run --no-build invocations corrupt the
# build.ninja file in the shared build/ dir; the slower ones die.
BUILT_TARGETS=()
ensure_built() {
  local target="$1"
  for t in "${BUILT_TARGETS[@]:-}"; do
    [[ "$t" == "$target" ]] && return 0
  done
  echo "[run_tests] Pre-building $target (avoids parallel cmake race)…"
  (cd "$NS3_ROOT" && ./ns3 build "$target") || {
    echo "[run_tests] WARN: ./ns3 build $target failed; falling back to per-run build." >&2
    return 1
  }
  BUILT_TARGETS+=("$target")
}

run_one() {
  local label="$1"
  local slot="${2:-0}"
  build_args
  if $ML && topology_supports_flag "$TOPOLOGY" mlEndpoint; then
    NS3_ARGS+=("--mlEndpoint=$(ml_endpoint_for_slot "$slot")")
  fi
  # train-mode per-slot section injection. cmd_train sets _SECTION_ID via
  # env-var prefix in the dispatch_parallel job string; traffic_mode,
  # failures, cripple, seed are global overrides picked up by build_args.
  if [[ -n "${_SECTION_ID:-}" ]]; then
    NS3_ARGS+=("--sections=$SECTIONS")
    NS3_ARGS+=("--sectionId=$_SECTION_ID")
  fi
  local logfile="$LOG_DIR/$(date +%Y%m%d-%H%M%S)-${label}.log"
  (( WORKERS > 1 )) && logfile="$LOG_DIR/$(date +%Y%m%d-%H%M%S)-${label}-w${slot}.log"
  local cmd="$TOPOLOGY ${NS3_ARGS[*]}"

  local nobuild_flag=""
  for t in "${BUILT_TARGETS[@]:-}"; do
    [[ "$t" == "$TOPOLOGY" ]] && nobuild_flag="--no-build" && break
  done

  echo "════════════════════════════════════════════════════════════════"
  echo "[run_tests] $label (slot=$slot)"
  echo "[run_tests] ./ns3 run $nobuild_flag \"$cmd\""
  echo "[run_tests] → $logfile"
  echo "════════════════════════════════════════════════════════════════"

  (
    cd "$NS3_ROOT"
    if $NS3_VERBOSE; then
      NS_LOG="ZmqOpenFlowController=level_info|prefix_time" ./ns3 run $nobuild_flag "$cmd"
    else
      ./ns3 run $nobuild_flag "$cmd"
    fi
  ) 2>&1 | tee "$logfile"

  # Scope metrics to exactly this run's files.
  # - train:  each run_one is one worker → its own train/w<slot>/ dir
  # - deploy: all slots for this run live under deploy/$RUN_ID/ → pass
  #           that dir so aggregate_metrics pools s*/metrics.csv for this
  #           run only (never mixes in files from other concurrent runs)
  # - flat/baseline: ML=false, so summarize_log skips metrics entirely
  local metrics_override=""
  if $ML; then
    case "$ML_LAYOUT" in
      train)  metrics_override="$(ml_agent_dir_for_slot "$slot")" ;;
      deploy)
        local _mbase; _mbase=$(ckpt_dir_for_priority "$PRIORITY")
        metrics_override="$_mbase/deploy/$RUN_ID" ;;
    esac
  fi
  summarize_log "$logfile" "$label" "$metrics_override"
}

# Run jobs in parallel batches of $WORKERS. Each job is an eval'd bash string;
# the slot index is supplied as the first positional arg.
dispatch_parallel() {
  local jobs=("$@")
  local total=${#jobs[@]}
  local idx=0
  while [[ $idx -lt $total ]]; do
    local pids=()
    local s=0
    while [[ $s -lt $WORKERS && $idx -lt $total ]]; do
      ( eval "${jobs[$idx]}" "$s" ) &
      pids+=($!)
      s=$((s + 1)); idx=$((idx + 1))
    done
    local pid
    for pid in "${pids[@]}"; do wait "$pid" || true; done
  done
}

# ----- mode implementations --------------------------------------------------
# Read section count from the registry (defaults to 1 for unknown topos).
topology_section_count() {
  local n; n=$(topology_cap "$1" sections)
  echo "${n:-1}"
}

cmd_single() {
  if $MULTI_CONTROLLER && (( SECTIONS == 1 )); then
    SECTIONS=$(topology_section_count "$TOPOLOGY")
    if (( SECTIONS > 1 )); then
      WORKERS=$SECTIONS
      EXTRA_ARGS+=("--sections=$SECTIONS")
      ML_LAYOUT="deploy"
      init_run_id
      ML_PORT_BASE=$(pick_free_port_base "$WORKERS")
      seed_deploy_dirs
    fi
  fi
  ensure_deploy_layout   # plain --ml single runs: keep priority root clean
  if $ML || $MULTI_CONTROLLER; then start_ml_service; fi
  local tag
  if $MULTI_CONTROLLER; then tag="multi"
  elif $ML; then tag="ml-${PRIORITY}"
  else tag="baseline"; fi
  run_one "${TOPOLOGY}-${tag}-${TRAFFIC_MODE}-seed${SEED}"
}

cmd_compare() {
  local _save_ml=$ML; ML=true; ensure_deploy_layout; ML=$_save_ml
  start_ml_service
  local save_ml=$ML
  ML=false
  run_one "${TOPOLOGY}-baseline-${TRAFFIC_MODE}-seed${SEED}"
  ML=true
  run_one "${TOPOLOGY}-ml-${PRIORITY}-${TRAFFIC_MODE}-seed${SEED}"
  ML=$save_ml
}

cmd_presets() {
  local _save_ml=$ML; ML=true; ensure_deploy_layout; ML=$_save_ml
  start_ml_service
  ML=false
  run_one "${TOPOLOGY}-baseline-${TRAFFIC_MODE}-seed${SEED}"
  local save_priority=$PRIORITY
  for p in balanced delay_first energy_first; do
    ML=true
    PRIORITY=$p
    run_one "${TOPOLOGY}-ml-${p}-${TRAFFIC_MODE}-seed${SEED}"
  done
  PRIORITY=$save_priority
}

cmd_seeds() {
  ensure_deploy_layout
  if $ML; then start_ml_service; fi
  local base=$SEED
  local jobs=()
  local tag
  tag="$($ML && echo "ml-${PRIORITY}" || echo "baseline")"
  for i in $(seq 0 $((N_SEEDS - 1))); do
    local seed=$((base + i))
    jobs+=("SEED=$seed run_one \"${TOPOLOGY}-${tag}-${TRAFFIC_MODE}-seed${seed}\"")
  done
  dispatch_parallel "${jobs[@]}"
  SEED=$base
}

cmd_matrix() {
  local _save_ml=$ML; ML=true; ensure_deploy_layout; ML=$_save_ml
  start_ml_service
  local base_seed=$SEED
  local jobs=()
  for mode in central random grouped; do
    for fail in true false; do
      for i in $(seq 0 $((N_SEEDS - 1))); do
        local seed=$((base_seed + i))
        jobs+=("ML=false TRAFFIC_MODE=$mode FAILURES=$fail SEED=$seed run_one \"${TOPOLOGY}-baseline-${mode}-fail${fail}-seed${seed}\"")
        jobs+=("ML=true  TRAFFIC_MODE=$mode FAILURES=$fail SEED=$seed run_one \"${TOPOLOGY}-ml-${PRIORITY}-${mode}-fail${fail}-seed${seed}\"")
      done
    done
  done
  dispatch_parallel "${jobs[@]}"
}

cmd_eval() {
  ML=true
  EXPLORE=false
  LEARN=false
  RESUME=true
  ensure_deploy_layout
  start_ml_service
  run_one "${TOPOLOGY}-eval-${PRIORITY}-${TRAFFIC_MODE}-seed${SEED}"
}

# Expand a --mix spec "topoA:countA,topoB,topoC:countC" into the global array
# MIX_TOPOS (one entry per worker, in spec order). A bare entry (no :count)
# gets an equal share of the leftover --workers budget (remainder handed to the
# earliest bare entries). Every topology is validated against the registry.
parse_mix() {
  local spec="$1" total_budget="${2:-0}"
  MIX_TOPOS=()
  local -a entries; IFS=',' read -ra entries <<< "$spec"
  # Pass 1 — tally explicit worker counts and bare entries.
  local explicit_sum=0 nbare=0 e topo cnt
  for e in "${entries[@]}"; do
    e="${e// /}"; [[ -z "$e" ]] && continue
    topo="${e%%:*}"
    [[ -n "$(topology_cap "$topo" sections)" ]] || {
      echo "[run_tests] ERROR: --mix unknown topology '$topo'" >&2; exit 1; }
    if [[ "$e" == *:* ]]; then
      cnt="${e##*:}"
      [[ "$cnt" =~ ^[0-9]+$ ]] || { echo "[run_tests] ERROR: --mix bad count in '$e'" >&2; exit 1; }
      explicit_sum=$(( explicit_sum + cnt ))
    else
      nbare=$(( nbare + 1 ))
    fi
  done
  local remaining=$(( total_budget - explicit_sum )); (( remaining < 0 )) && remaining=0
  local base=0 extra=0
  if (( nbare > 0 )); then
    (( remaining >= nbare )) || {
      echo "[run_tests] ERROR: --mix has $nbare bare entries but only $remaining leftover workers (raise --workers)" >&2
      exit 1; }
    base=$(( remaining / nbare )); extra=$(( remaining % nbare ))
  fi
  # Pass 2 — emit one entry per worker, preserving spec order.
  local bare_i=0 c share
  for e in "${entries[@]}"; do
    e="${e// /}"; [[ -z "$e" ]] && continue
    topo="${e%%:*}"
    if [[ "$e" == *:* ]]; then
      share="${e##*:}"
    else
      share=$base; (( bare_i < extra )) && share=$(( share + 1 )); bare_i=$(( bare_i + 1 ))
    fi
    for ((c=0; c<share; c++)); do MIX_TOPOS+=("$topo"); done
  done
}

# Unified federated training. --sections N --workers M --rounds R spawns
# N*M workers in parallel (M replicas per section), federated via shared dir.
# sections=1 = all workers run the full topology with seed+workerId for diversity.
# sections>1 = workers split across sections (workerId/M = sectionId).
#
# Each worker is also assigned a CURRICULUM VARIANT (traffic mode × failures ×
# cripple) from topology_variants. Variants are round-robined globally across
# (round, worker) so if total_workers < num_variants the next round picks up
# where the previous one left off, guaranteeing every variant is covered.
cmd_train() {
  # --mix → heterogeneous pool (localized critic makes this safe): one worker
  # per MIX_TOPOS entry, each running its own full topology (sections=1) but
  # federating its actor into one shared pool. Otherwise the classic single-
  # topology path (SECTIONS × WORKERS).
  local MIXED=false
  declare -A VAR_BY_TOPO=() TOPO_VAR_IDX=()
  local workers_per_section total
  if [[ -n "$MIX" ]]; then
    MIXED=true
    SECTIONS=1
    parse_mix "$MIX" "$WORKERS"
    total=${#MIX_TOPOS[@]}
    (( total >= 1 )) || { echo "[run_tests] ERROR: --mix produced no workers" >&2; exit 1; }
    # Per-topology variant cache (round-robined within each topology's workers)
    # and a pre-build of every distinct binary (avoids the parallel cmake race).
    local _t _v _vs
    for _t in "${MIX_TOPOS[@]}"; do
      if [[ -z "${VAR_BY_TOPO[$_t]:-}" ]]; then
        _vs=""
        while IFS= read -r _v; do [[ -n "$_v" ]] && _vs+="$_v "; done < <(topology_variants "$_t")
        VAR_BY_TOPO[$_t]="$_vs"
        TOPO_VAR_IDX[$_t]=0
        ensure_built "$_t"
      fi
    done
  else
    (( SECTIONS >= 1 )) || { echo "[run_tests] ERROR: --sections must be >= 1" >&2; exit 1; }
    workers_per_section=$WORKERS
    (( workers_per_section >= 1 )) || { echo "[run_tests] ERROR: --workers must be >= 1" >&2; exit 1; }
    total=$((SECTIONS * workers_per_section))
  fi

  ML=true
  EXPLORE=true
  LEARN=true
  RESUME=true
  MIXED_LOAD=true           # need traffic for the agent to learn from
  WORKERS=$total            # ml services + dispatch use the total
  ML_LAYOUT="train"         # workers write to $base/train/w<id>/
  FEDAVG_DIR="$SCRIPT_DIR/data/federated_weights/$PRIORITY"
  FEDAVG_AGG_LOG_DIR="$SCRIPT_DIR/data/fedavg/$PRIORITY"

  # Build curriculum variant list from the topology capability registry.
  # (mixed mode already built a per-topology cache above.)
  local variants=() num_variants=0
  if ! $MIXED; then
    while IFS= read -r v; do
      [[ -n "$v" ]] && variants+=("$v")
    done < <(topology_variants "$TOPOLOGY")
    num_variants=${#variants[@]}
    (( num_variants > 0 )) || {
      echo "[run_tests] ERROR: $TOPOLOGY has no train variants (check topology_cap)" >&2
      exit 1
    }
  fi

  if $MIXED; then
    echo "[run_tests] Federated MIXED-topology training: priority=$PRIORITY"
    echo "[run_tests]   pool=$total workers → ${MIX_TOPOS[*]}"
    echo "[run_tests]   rounds=$FED_ROUNDS, fedavg_every=$FEDAVG_EVERY steps, timeout=${FEDAVG_TIMEOUT}s"
    echo "[run_tests]   weights_dir=$FEDAVG_DIR  (mode=$( $FED_RESET && echo reset || echo resume ))"
    echo "[run_tests]   actor federated across the pool; each worker keeps its own local critic"
    echo "[run_tests]   per-topology curriculum round-robined within each topology's workers"
  else
    echo "[run_tests] Federated training: topology=$TOPOLOGY priority=$PRIORITY"
    echo "[run_tests]   sections=$SECTIONS, workers/section=$workers_per_section, total=$total"
    echo "[run_tests]   rounds=$FED_ROUNDS, fedavg_every=$FEDAVG_EVERY steps, timeout=${FEDAVG_TIMEOUT}s"
    echo "[run_tests]   weights_dir=$FEDAVG_DIR  (mode=$( $FED_RESET && echo reset || echo resume ))"
    echo "[run_tests]   curriculum variants ($num_variants), round-robined across all (round, worker) slots:"
    for v in "${variants[@]}"; do
      IFS='|' read -r vm vf vc <<< "$v"
      echo "[run_tests]     mode=$vm failures=$vf cripple=$vc"
    done
  fi

  mkdir -p "$FEDAVG_DIR" "$FEDAVG_AGG_LOG_DIR"

  # Resume logic:
  #   --reset  → wipe both FedAvg dir and per-worker checkpoints, start fresh
  #   default  → preserve global_round_*.pt and per-worker local.pt; the
  #              aggregator continues at max(global_round_*) + 1.
  # Either way, drop orphan worker_*.pt files left by a prior crashed round
  # so the aggregator doesn't count them toward the next round.
  if $FED_RESET; then
    echo "[run_tests]   --reset: wiping $FEDAVG_DIR and $CKPT_ROOT/$PRIORITY"
    rm -f "$FEDAVG_DIR"/*.pt "$FEDAVG_DIR"/*.pt.tmp 2>/dev/null || true
    rm -rf "$CKPT_ROOT/$PRIORITY" 2>/dev/null || true
    mkdir -p "$CKPT_ROOT/$PRIORITY"
  else
    # Drop orphan worker submissions from a previous crashed round; keep
    # global_round_*.pt history.
    rm -f "$FEDAVG_DIR"/worker_*.pt "$FEDAVG_DIR"/*.pt.tmp 2>/dev/null || true
    # Latest global checkpoint (if any) — used to bootstrap new workers.
    local latest_global=""
    latest_global=$(ls -v "$FEDAVG_DIR"/global_round_*.pt 2>/dev/null | tail -1 || true)
    if [[ -n "$latest_global" ]]; then
      local last_round
      last_round=$(basename "$latest_global" | sed -E 's/global_round_([0-9]+)\.pt/\1/')
      echo "[run_tests]   resume: latest global = round $last_round ($latest_global)"
      # Bootstrap any worker that doesn't have a local.pt yet (e.g. new
      # worker slots when --workers grew between invocations) by copying
      # the latest global model in. Workers with an existing local.pt are
      # left alone — their local checkpoint is already post-FedAvg-state.
      local s base
      for ((s=0; s<total; s++)); do
        base=$(ml_agent_dir_for_slot "$s")
        mkdir -p "$base"
        if [[ ! -f "$base/local.pt" ]]; then
          cp "$latest_global" "$base/local.pt"
          echo "[run_tests]   bootstrap slot $s from $(basename "$latest_global") → $base/local.pt"
        fi
      done
    else
      echo "[run_tests]   resume: no prior global checkpoint — starting fresh"
    fi
  fi

  start_aggregator
  start_ml_service

  local base_seed=$SEED
  for ((round=1; round<=FED_ROUNDS; round++)); do
    echo
    echo "[run_tests] === Training round $round / $FED_ROUNDS ==="
    local jobs=()
    for ((w=0; w<total; w++)); do
      local seed=$((base_seed + (round - 1) * total + w))
      local var vm vf vc fail_str crip_str label
      if $MIXED; then
        # Worker w is permanently bound to MIX_TOPOS[w] (stable across rounds),
        # so its local critic stays calibrated to one topology. Variant is
        # round-robined within that topology's own workers.
        local wtopo="${MIX_TOPOS[$w]}"
        local -a _tv=(${VAR_BY_TOPO[$wtopo]})
        local _n=${#_tv[@]} _vi=${TOPO_VAR_IDX[$wtopo]}
        var="${_tv[$(( _vi % _n ))]}"
        TOPO_VAR_IDX[$wtopo]=$(( _vi + 1 ))
        IFS='|' read -r vm vf vc <<< "$var"
        fail_str=false; [[ "$vf" == "1" ]] && fail_str=true
        crip_str=false; [[ "$vc" == "1" ]] && crip_str=true
        label="${wtopo}-mixtrain-w${w}-${vm}-f${vf}c${vc}-seed${seed}-r${round}"
        # Override TOPOLOGY per job (run_one uses $TOPOLOGY as the ns-3 binary);
        # each worker runs its full topology (sections=1, sectionId=0).
        jobs+=("TOPOLOGY=$wtopo _SECTION_ID=0 TRAFFIC_MODE=$vm FAILURES=$fail_str CRIPPLE=$crip_str SEED=$seed run_one \"$label\"")
      else
        local sid=$((w / workers_per_section))
        local rep=$((w % workers_per_section))
        # Round-robin curriculum across all (round, worker) slots.
        local global_idx=$(( (round - 1) * total + w ))
        var="${variants[$((global_idx % num_variants))]}"
        IFS='|' read -r vm vf vc <<< "$var"
        fail_str=false; [[ "$vf" == "1" ]] && fail_str=true
        crip_str=false; [[ "$vc" == "1" ]] && crip_str=true
        label="${TOPOLOGY}-train-s${sid}r${rep}-${vm}-f${vf}c${vc}-seed${seed}-r${round}"
        jobs+=("_SECTION_ID=$sid TRAFFIC_MODE=$vm FAILURES=$fail_str CRIPPLE=$crip_str SEED=$seed run_one \"$label\"")
      fi
    done
    dispatch_parallel "${jobs[@]}"
  done

  echo
  echo "[run_tests] Training complete ($FED_ROUNDS rounds, $total workers/round)."
  echo "[run_tests]   Aggregator round log: $FEDAVG_AGG_LOG_DIR/rounds.csv"

  # Publish the canonical model: latest global_round_*.pt → $base/local.pt.
  # This is what eval / single / --multiController will load.
  local base; base=$(ckpt_dir_for_priority "$PRIORITY")
  local latest; latest=$(ls -v "$FEDAVG_DIR"/global_round_*.pt 2>/dev/null | tail -1 || true)
  if [[ -n "$latest" ]]; then
    mkdir -p "$base"
    cp "$latest" "$base/local.pt"
    echo "[run_tests]   Published canonical model: $base/local.pt (← $(basename "$latest"))"
  else
    echo "[run_tests]   WARN: no global_round_*.pt produced; $base/local.pt untouched." >&2
  fi
}

cmd_summary() {
  local f="${1:-}"
  [[ -n "$f" && -f "$f" ]] || { echo "summary: file not found: $f" >&2; exit 1; }
  summarize_log "$f" "$(basename "$f" .log)"
}

cmd_clean() {
  local priority_dir; priority_dir=$(ckpt_dir_for_priority "$PRIORITY")
  local fedavg_dir="$SCRIPT_DIR/data/federated_weights/$PRIORITY"
  echo "[run_tests] Removing $priority_dir (priority=$PRIORITY)"
  rm -rf "$priority_dir" 2>/dev/null || true
  echo "[run_tests] Removing $fedavg_dir"
  rm -rf "$fedavg_dir" 2>/dev/null || true
  echo "[run_tests] Done. Next --priority=$PRIORITY run starts fresh."
  echo "[run_tests] (other priorities under $CKPT_ROOT are untouched)"
}

# ----- arg parser ------------------------------------------------------------
[[ $# -lt 1 ]] && { usage; exit 0; }
case "$1" in -h|--help) usage; exit 0 ;; esac
MODE="$1"; shift

if [[ "$MODE" == "summary" ]]; then
  cmd_summary "${1:-}"
  exit $?
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --topology)     TOPOLOGY="$2"; shift 2 ;;
    --simTime)      SIM_TIME="$2"; shift 2 ;;
    --warmupS)      WARMUP="$2"; shift 2 ;;
    --trafficMode)  TRAFFIC_MODE="$2"; shift 2 ;;
    --seed)         SEED="$2"; shift 2 ;;
    --seeds)        N_SEEDS="$2"; shift 2 ;;
    --priority)     PRIORITY="$2"; shift 2 ;;
    --evalWindowS)  EVAL_WINDOW="$2"; shift 2 ;;
    --ml)           ML=true; shift ;;
    --no-ml)        ML=false; shift ;;
    --mixedLoad)    MIXED_LOAD=true; shift ;;
    --no-mixedLoad) MIXED_LOAD=false; shift ;;
    --failures)     FAILURES=true; shift ;;
    --no-failures)  FAILURES=false; shift ;;
    --cripple)      CRIPPLE=true; shift ;;
    --no-cripple)   CRIPPLE=false; shift ;;
    --multiController) MULTI_CONTROLLER=true; shift ;;
    --no-explore)   EXPLORE=false; shift ;;
    --no-learn)     LEARN=false; shift ;;
    --no-resume)    RESUME=false; shift ;;
    --noiseSigma)   NOISE_SIGMA="$2"; shift 2 ;;
    --no-auto-ml)   AUTO_ML=false; shift ;;
    --verbose)      NS3_VERBOSE=true; shift ;;
    --workers)      WORKERS="$2"; shift 2 ;;
    --sections)     SECTIONS="$2"; shift 2 ;;
    --mix)          MIX="$2"; shift 2 ;;
    --rounds)       FED_ROUNDS="$2"; shift 2 ;;
    --reset)        FED_RESET=true; shift ;;
    --fedAvgEverySteps) FEDAVG_EVERY="$2"; shift 2 ;;
    --fedAvgTimeoutS)   FEDAVG_TIMEOUT="$2"; shift 2 ;;
    -h|--help)      usage; exit 0 ;;
    --)             shift; EXTRA_ARGS=("$@"); break ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

# --topology is required for every mode except clean (which is priority-scoped,
# not topology-scoped).
if [[ "$MODE" != "clean" && -z "$TOPOLOGY" && -z "$MIX" ]]; then
  echo "[run_tests] ERROR: --topology is required (usa | fat-tree-k4 | sensor-cluster | two-switch-ping)" >&2
  exit 1
fi

# train requires explicit --sections and --workers — defaulting them silently
# would hide intent and risk training the wrong fleet size. With --mix (a
# heterogeneous pool) --sections is irrelevant; --workers is the total pool /
# even-split budget.
if [[ "$MODE" == "train" ]]; then
  if [[ -n "$MIX" ]]; then
    [[ -z "$WORKERS" ]] && { echo "[run_tests] ERROR: --mix requires --workers M (total pool / split budget)" >&2; exit 1; }
  else
    [[ -z "$SECTIONS" ]] && { echo "[run_tests] ERROR: train mode requires --sections N" >&2; exit 1; }
    [[ -z "$WORKERS"  ]] && { echo "[run_tests] ERROR: train mode requires --workers M" >&2; exit 1; }
  fi
fi

# For all other modes, fall back to single-process / single-section defaults.
SECTIONS="${SECTIONS:-1}"
WORKERS="${WORKERS:-1}"

if [[ "$MODE" != "clean" && -z "$MIX" ]]; then
  ensure_built "$TOPOLOGY"
fi

case "$MODE" in
  single)    cmd_single ;;
  compare)   cmd_compare ;;
  presets)   cmd_presets ;;
  seeds)     cmd_seeds ;;
  matrix)    cmd_matrix ;;
  eval)      cmd_eval ;;
  train)     cmd_train ;;
  clean)     cmd_clean; exit 0 ;;
  *) echo "Unknown mode: $MODE" >&2; usage; exit 1 ;;
esac

echo
echo "[run_tests] Done. Summary CSV: $SUMMARY_CSV"
