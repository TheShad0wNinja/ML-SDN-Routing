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
#   eval       pretrained eval-only run (--mlExplore=false --mlResume=true)
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
RESUME=true
N_SEEDS=5
EVAL_WINDOW=0
EXTRA_ARGS=()
AUTO_ML=true
NS3_VERBOSE=false
ML_PORT_BASE=5555

# Federated training (used by `train` mode).
FED_ROUNDS=1              # back-to-back rounds in one invocation
FED_RESET=false           # --reset: wipe priority's training state before training
FEDAVG_DIR=""
FEDAVG_EVERY=0
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
  eval                  ML run with --mlExplore=false --mlResume=true
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
  --topology X          usa | fat-tree-k4 | two-switch-ping        (REQUIRED)
                        Note: fat-tree-k4 has 1 section; --multiController
                        and --sections>1 are unsupported there.
  --simTime N           Simulation duration (s)                    (default: 600)
  --warmupS N           Warmup window                              (default: 10)
  --trafficMode X       random | central | grouped                 (default: central)
  --seed N              Base random seed                           (default: 12345)
  --seeds N             Number of seeds for seeds/matrix modes     (default: 5)
  --priority X          balanced | throughput | energy | custom (default: balanced)
                        Each priority has its own checkpoint dir under
                        scratch/data/agent/<priority>/ — train all three
                        independently and switch between them via this flag.
  --ml | --no-ml        Enable / disable the ML controller         (default: off)
  --mixedLoad | --no-mixedLoad
                        Toggle mixed-protocol (TCP+UDP) background load
                                                                   (default: on)
  --failures | --no-failures   Toggle scheduled link churn         (default: off)
  --cripple  | --no-cripple    Toggle Missoula crippling (USA)     (default: off)
  --multiController     Run M in-process controllers using trained weights
  --no-explore          Disable OU noise & gradient updates
  --no-resume           Don't resume the ML agent from checkpoint
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
  if (( WORKERS <= 1 )); then echo "$base"
  else echo "$base/w$1"; fi
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
      echo "[run_tests] ML service already on :$port — reusing (slot $s, dir $dir)."
      WORKER_ML_PIDS[$s]=""
      continue
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
    ml|mlPriority|mlExplore|mlResume|mlEndpoint|mlPortBase|multiController|sections|sectionId)
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
    if [[ "$RESUME"  == "false" ]]; then add_if_supported mlResume  false; fi
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

summarize_log() {
  local f="$1" label="$2"
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

  local jpermb=""
  if [[ -n "$energy" && -n "$rx" && "$rx" != "0" ]]; then
    # Assume ~1KB avg payload — same approximation across runs makes this
    # comparable; absolute number is not the point.
    jpermb=$(awk -v e="$energy" -v r="$rx" 'BEGIN{printf "%.3f", e/(r*1024*8/1e6)}')
  fi

  local ml_ticks ml_reward_final ml_reward_mean ml_critic_loss ml_actor_loss
  ml_ticks=""; ml_reward_final=""; ml_reward_mean=""
  ml_critic_loss=""; ml_actor_loss=""
  local metrics_csv; metrics_csv="$(ckpt_dir_for_priority "$PRIORITY")/metrics.csv"
  if $ML && [[ -f "$metrics_csv" ]]; then
    local agg
    agg=$(python3 "$SCRIPT_DIR/python/aggregate_metrics.py" \
            "$metrics_csv" 2>/dev/null || true)
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
  local expected_header="timestamp,label,topology,sim_time_s,traffic_mode,seed,controller,priority,mixed_load,failures,cripple,ping_success_pct,rtt_avg_ms,rtt_jitter_ms,pdr_pct,e2e_delay_avg_ms,flows,tx_pkts,rx_pkts,hop_count_avg,energy_total_j,energy_residual_j,power_avg_w,per_sw_consumed_j,per_sw_residual_j,residual_pct,j_per_mb,ml_reward_final,ml_reward_mean_last25,ml_critic_loss_final,ml_actor_loss_final,ml_ticks"
  if [[ -f "$SUMMARY_CSV" ]] && [[ "$(head -n1 "$SUMMARY_CSV")" != "$expected_header" ]]; then
    local archived="${SUMMARY_CSV%.csv}.$(date +%Y%m%d-%H%M%S).csv"
    mv "$SUMMARY_CSV" "$archived"
    echo "[run_tests] CSV schema changed — archived old rows to $archived"
  fi
  if [[ ! -f "$SUMMARY_CSV" ]]; then
    echo "$expected_header" >"$SUMMARY_CSV"
  fi
  printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
    "$(date -Iseconds)" "$label" "$TOPOLOGY" "$SIM_TIME" "$TRAFFIC_MODE" "$SEED" \
    "$($ML && echo ml || echo baseline)" "$($ML && echo "$PRIORITY" || echo -)" \
    "$($MIXED_LOAD && echo 1 || echo 0)" "$($FAILURES && echo 1 || echo 0)" "$($CRIPPLE && echo 1 || echo 0)" \
    "${success:-}" "${rtt:-}" "${jitter:-}" "${delivery:-}" "${delay:-}" \
    "${flows:-}" "${tx:-}" "${rx:-}" "${hops:-}" \
    "${energy:-}" "${residual:-}" "${power:-}" \
    "${per_sw_consumed:-}" "${per_sw_residual:-}" "${residual_frac:-}" "${jpermb:-}" \
    "${ml_reward_final:-}" "${ml_reward_mean:-}" "${ml_critic_loss:-}" "${ml_actor_loss:-}" \
    "${ml_ticks:-}" \
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

  summarize_log "$logfile" "$label"
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
    fi
  fi
  if $ML || $MULTI_CONTROLLER; then start_ml_service; fi
  local tag
  if $MULTI_CONTROLLER; then tag="multi"
  elif $ML; then tag="ml-${PRIORITY}"
  else tag="baseline"; fi
  run_one "${TOPOLOGY}-${tag}-${TRAFFIC_MODE}-seed${SEED}"
}

cmd_compare() {
  start_ml_service
  local save_ml=$ML
  ML=false
  run_one "${TOPOLOGY}-baseline-${TRAFFIC_MODE}-seed${SEED}"
  ML=true
  run_one "${TOPOLOGY}-ml-${PRIORITY}-${TRAFFIC_MODE}-seed${SEED}"
  ML=$save_ml
}

cmd_presets() {
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
  RESUME=true
  start_ml_service
  run_one "${TOPOLOGY}-eval-${PRIORITY}-${TRAFFIC_MODE}-seed${SEED}"
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
  (( SECTIONS >= 1 )) || { echo "[run_tests] ERROR: --sections must be >= 1" >&2; exit 1; }
  local workers_per_section=$WORKERS
  (( workers_per_section >= 1 )) || { echo "[run_tests] ERROR: --workers must be >= 1" >&2; exit 1; }
  local total=$((SECTIONS * workers_per_section))

  ML=true
  EXPLORE=true
  RESUME=true
  MIXED_LOAD=true           # need traffic for the agent to learn from
  WORKERS=$total            # ml services + dispatch use the total
  FEDAVG_DIR="$SCRIPT_DIR/data/federated_weights/$PRIORITY"
  FEDAVG_AGG_LOG_DIR="$SCRIPT_DIR/data/fedavg/$PRIORITY"

  # Build curriculum variant list from the topology capability registry.
  local variants=()
  while IFS= read -r v; do
    [[ -n "$v" ]] && variants+=("$v")
  done < <(topology_variants "$TOPOLOGY")
  local num_variants=${#variants[@]}
  (( num_variants > 0 )) || {
    echo "[run_tests] ERROR: $TOPOLOGY has no train variants (check topology_cap)" >&2
    exit 1
  }

  echo "[run_tests] Federated training: topology=$TOPOLOGY priority=$PRIORITY"
  echo "[run_tests]   sections=$SECTIONS, workers/section=$workers_per_section, total=$total"
  echo "[run_tests]   rounds=$FED_ROUNDS, fedavg_every=$FEDAVG_EVERY steps, timeout=${FEDAVG_TIMEOUT}s"
  echo "[run_tests]   weights_dir=$FEDAVG_DIR  (mode=$( $FED_RESET && echo reset || echo resume ))"
  echo "[run_tests]   curriculum variants ($num_variants), round-robined across all (round, worker) slots:"
  for v in "${variants[@]}"; do
    IFS='|' read -r vm vf vc <<< "$v"
    echo "[run_tests]     mode=$vm failures=$vf cripple=$vc"
  done

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
      local sid=$((w / workers_per_section))
      local rep=$((w % workers_per_section))
      local seed=$((base_seed + (round - 1) * total + w))
      # Round-robin curriculum across all (round, worker) slots.
      local global_idx=$(( (round - 1) * total + w ))
      local var="${variants[$((global_idx % num_variants))]}"
      IFS='|' read -r vm vf vc <<< "$var"
      local fail_str=false; [[ "$vf" == "1" ]] && fail_str=true
      local crip_str=false; [[ "$vc" == "1" ]] && crip_str=true
      local label="${TOPOLOGY}-train-s${sid}r${rep}-${vm}-f${vf}c${vc}-seed${seed}-r${round}"
      jobs+=("_SECTION_ID=$sid TRAFFIC_MODE=$vm FAILURES=$fail_str CRIPPLE=$crip_str SEED=$seed run_one \"$label\"")
    done
    dispatch_parallel "${jobs[@]}"
  done

  echo
  echo "[run_tests] Training complete ($FED_ROUNDS rounds, $total workers/round)."
  echo "[run_tests]   Aggregator round log: $FEDAVG_AGG_LOG_DIR/rounds.csv"
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
    --no-resume)    RESUME=false; shift ;;
    --no-auto-ml)   AUTO_ML=false; shift ;;
    --verbose)      NS3_VERBOSE=true; shift ;;
    --workers)      WORKERS="$2"; shift 2 ;;
    --sections)     SECTIONS="$2"; shift 2 ;;
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
if [[ "$MODE" != "clean" && -z "$TOPOLOGY" ]]; then
  echo "[run_tests] ERROR: --topology is required (usa | fat-tree-k4 | two-switch-ping)" >&2
  exit 1
fi

# train requires explicit --sections and --workers — defaulting them silently
# would hide intent and risk training the wrong fleet size.
if [[ "$MODE" == "train" ]]; then
  [[ -z "$SECTIONS" ]] && { echo "[run_tests] ERROR: train mode requires --sections N" >&2; exit 1; }
  [[ -z "$WORKERS"  ]] && { echo "[run_tests] ERROR: train mode requires --workers M" >&2; exit 1; }
fi

# For all other modes, fall back to single-process / single-section defaults.
SECTIONS="${SECTIONS:-1}"
WORKERS="${WORKERS:-1}"

if [[ "$MODE" != "clean" ]]; then
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
