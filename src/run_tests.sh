#!/usr/bin/env bash
# scratch/run_tests.sh — drive ns-3 SDN test variants.
#
# Modes:
#   single     one run with whatever flags you pass
#   compare    baseline + one ML run, same seed/params
#   presets    baseline + balanced + delay_first + energy_first ML runs
#   seeds      repeat the same config across N consecutive seeds
#   matrix     full statistical matrix (seeds × trafficMode × failures, ml vs baseline)
#   train      curriculum-style training: cycle the agent through traffic
#              modes × failures × cripple, persisting weights across runs
#   eval       pretrained eval-only run (--mlExplore=false --mlResume=true)
#   federated  Hierarchical SDN Phase 1 — M ns-3 processes (one per section
#              of sections.json), each its own Local Controller + RL agent,
#              FedAvg'd by root_aggregator.py via scratch/data/federated_weights/
#   fullrun    Hierarchical SDN Phase 2 — ONE ns-3 process running the FULL
#              topology with M Local Controllers (one per section). Inter-
#              domain routing uses static border-switch flow-mods (Option A)
#              precomputed from sections.json. FedAvg is opt-in via
#              --fedAvgEverySteps; when on, reuses Phase 1's root_aggregator.
#   summary    parse a saved log and print headline numbers
#   clean      wipe checkpoints + replay buffer (forces fresh learning)
#
# Each ns3 run is teed to scratch/data/results/logs/ with a timestamped name,
# then summarized to scratch/data/results/summary.csv.
#
# Examples:
#   scratch/run_tests.sh compare --simTime 600 --trafficMode central --tcp --failures --cripple
#   scratch/run_tests.sh presets --simTime 600 --tcp --failures --cripple
#   scratch/run_tests.sh seeds --seeds 5 --ml --priority balanced --simTime 1200 --tcp --failures --cripple
#   scratch/run_tests.sh matrix --seeds 3 --simTime 600 --priority balanced
#   scratch/run_tests.sh eval --simTime 200 --priority balanced --tcp --failures --cripple
#   scratch/run_tests.sh federated --simTime 30 --warmupS 3 \
#       --fedAvgEverySteps 5 --fedAvgTimeoutS 60 -- --mlIntervalS=0.5
#   scratch/run_tests.sh fullrun --simTime 120 --ml --priority balanced \
#       --fedAvgEverySteps 200
#   scratch/run_tests.sh summary scratch/data/results/logs/<name>.log

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NS3_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RESULTS_DIR="$SCRIPT_DIR/data/results"
LOG_DIR="$RESULTS_DIR/logs"
SUMMARY_CSV="$RESULTS_DIR/summary.csv"
ML_SERVICE_PY="$SCRIPT_DIR/python/controller/ml_service.py"
CKPT_DIR="$SCRIPT_DIR/data/agent"

mkdir -p "$LOG_DIR"

# ----- defaults --------------------------------------------------------------
TOPOLOGY="usa"
SIM_TIME=600
WARMUP=10
TRAFFIC_MODE="central"
SEED=12345
PRIORITY="balanced"
ML=false
TCP=true
FAILURES=false
CRIPPLE=false
EXPLORE=true
RESUME=true
N_SEEDS=5
EVAL_WINDOW=0
EXTRA_ARGS=()
AUTO_ML=true
NS3_VERBOSE=false
WORKERS=1                 # parallel ns-3 processes (and ML services if --ml)
ML_PORT_BASE=5555         # worker w binds tcp://*:$((ML_PORT_BASE+w))

# Federated learning (Phase 1 of hierarchical SDN). Each worker simulates one
# section of the topology and the root aggregator FedAvgs their weights every
# FEDAVG_EVERY training steps. FEDAVG_EVERY=0 disables federation entirely
# (default — backward-compat with all existing modes).
FEDAVG_DIR=""
FEDAVG_EVERY=0
FEDAVG_TIMEOUT=300
FEDAVG_AGG_LOG_DIR=""
SECTIONS_JSON=""
AGG_PID=""
WE_STARTED_AGG=false

# `train` mode curriculum — overrideable via flags.
TRAIN_ROUNDS=2
TRAIN_MODES="central random grouped"
TRAIN_FAILURES="true false"
TRAIN_CRIPPLE="true false"

ML_PID=""
WE_STARTED_ML=false
# Parallel-mode bookkeeping: per-worker pids and dirs.
declare -a WORKER_ML_PIDS=()
declare -a WORKER_AGENT_DIRS=()
# Federated-mode bookkeeping: per-worker (= per-section) topology slice.
# Empty arrays = non-federated mode; run_one ignores these.
declare -a WORKER_SECTION_IDS=()
declare -a WORKER_SECTION_NODES=()

# ----- usage -----------------------------------------------------------------
usage() {
  cat <<'EOF'
Usage: scratch/run_tests.sh <mode> [options] [-- extra-ns3-args]

Modes:
  single                One run with the given flags
  compare               Baseline run + ML run, same seed/params
  presets               Baseline + 3 ML priority presets (balanced/delay/energy)
  seeds                 Sweep N consecutive seeds for one config
  matrix                Full matrix: seeds × {central,random,grouped} × {failures on/off} × {ml,baseline}
  train                 Curriculum-style training: cycle the agent through
                        {trafficMode × failures × cripple} variations with
                        --mlResume=true. Run this BEFORE compare/eval so the
                        agent is well-trained.
  eval                  ML run with --mlExplore=false --mlResume=true (deterministic eval)
  federated             Phase-1 hierarchical SDN: launch one ns-3 process per
                        topology section (read from sections.json), each with
                        its own Local Controller and RL agent. A root
                        aggregator FedAvgs all sections' weights every
                        --fedAvgEverySteps training steps via a shared
                        directory (scratch/data/federated_weights/).
  fullrun               Phase-2 hierarchical SDN: ONE ns-3 process running the
                        FULL topology with M Local Controllers (one per
                        section of sections.json). Inter-domain routing is
                        Option A — static border-switch flow-mods derived from
                        sections.json's inter_domain_routes block, pre-installed
                        after warmup. Each controller still runs its own
                        ml_service.py on its own port; pass
                        --fedAvgEverySteps to also FedAvg the M controllers'
                        weights via the same root_aggregator.py as Phase 1.
                        This is the defense/demo configuration — proves the
                        hierarchical architecture end-to-end on one box.
  summary FILE          Print headline stats from a saved log file
  clean                 Remove ML checkpoint + replay buffer

Options:
  --topology X          usa | usa-fullrun | abilene | mini-geant |
                        two-switch-ping                                (default: usa)
                        Note: 'fullrun' mode forces topology=usa-fullrun
  --simTime N           Simulation duration in seconds                 (default: 600)
  --warmupS N           Warmup window                                  (default: 10)
  --trafficMode X       random | central | grouped                     (default: central)
  --seed N              Base random seed                               (default: 12345)
  --seeds N             Number of seeds for 'seeds' / 'matrix' modes   (default: 5)
  --priority X          balanced | delay_first | energy_first | custom (default: balanced)
  --ml | --no-ml        Enable / disable the ML controller             (default: off)
  --tcp | --no-tcp      Toggle TCP background load                     (default: off)
  --failures | --no-failures   Toggle scheduled link churn             (default: off)
  --cripple  | --no-cripple    Toggle Missoula crippling (USA only)    (default: off)
  --no-explore          Disable OU noise & gradient updates (eval mode)
  --no-resume           Don't resume the ML agent from checkpoint
  --evalWindowS N       Delay FlowMonitor reset by N seconds past warmup
  --no-auto-ml          Don't auto-start the Python ML service
  --verbose             Pass NS_LOG to surface controller info
  --workers N           Parallel ns-3 processes (and per-worker ML services).
                        Only seeds + matrix modes actually parallelize; train
                        is sequential (curriculum needs ordering). With --ml
                        each worker w gets its own port (5555+w) and its own
                        checkpoint dir (scratch/data/agent-w<N>) — N
                        INDEPENDENT agents, not one shared agent.

Train-mode curriculum (defaults give 12 scenarios × 2 rounds = 24 runs):
  --trainRounds N       How many times to loop the whole curriculum (default 2)
  --trainModes "..."    Space-separated traffic modes (default: "central random grouped")
  --trainFailures "..." Space-separated bool values  (default: "true false")
  --trainCripple "..."  Space-separated bool values  (default: "true")

Federated / fullrun-mode flags:
  --sectionsJson PATH       Topology partition file. Used by BOTH 'federated'
                            (multi-process) and 'fullrun' (single-process)
                            modes (default: scratch/scenarios/usa/sections.json)
  --fedAvgEverySteps K      Workers submit weights every K training steps,
                            and block on the global model (default: 0 = off).
                            In 'federated' this is the cross-process FedAvg
                            cadence; in 'fullrun' it FedAvgs the in-process
                            controllers via the same shared directory.
  --fedAvgTimeoutS S        Per-round timeout: aggregator drops the round and
                            workers continue with local weights (default: 300)

  -- arg1 arg2 ...      Forward extra args verbatim to ns3 (after --)

Where logs go:   scratch/data/results/logs/
Summary CSV:     scratch/data/results/summary.csv
EOF
}

# ----- ML service lifecycle --------------------------------------------------
ml_port_for_slot()      { echo $((ML_PORT_BASE + $1)); }
ml_endpoint_for_slot()  { echo "tcp://127.0.0.1:$(ml_port_for_slot "$1")"; }
ml_agent_dir_for_slot() {
  if (( WORKERS <= 1 )); then echo "$CKPT_DIR"
  else echo "${CKPT_DIR}-w$1"; fi
}

ml_port_listening() {
  # True only when the python service is actually pumping its REP loop, not
  # just bound. A TCP-connect check returns true the instant bind() runs,
  # but service.run() may still be inside the torch import — sending a real
  # ZMQ request and waiting for any reply forces us to wait until the
  # dispatch loop is alive.
  local port="$1"
  python3 - "$port" <<'PY' 2>/dev/null
import sys
try:
    import zmq
except ImportError:
    # No pyzmq available — fall back to TCP-connect probe (less reliable).
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
    # get_action is a cheap idempotent request; service returns a zero-vector
    # if no agent is initialized yet — that's fine, we only need any reply.
    sock.send(b'{"cmd":"get_action"}')
    sock.recv()
    sys.exit(0)
except Exception:
    sys.exit(1)
finally:
    sock.close(0); ctx.term()
PY
}

# Start one ML service per worker slot. Each slot gets its own port + dir.
# Existing service on a slot's port is reused.
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

  # Wait for every slot we started to bind. Torch import is the slow part on
  # cold start (~10s); allow 30s.
  local s elapsed=0 max=30 all_ready=false
  while (( elapsed < max )); do
    all_ready=true
    for ((s=0; s<WORKERS; s++)); do
      local pid="${WORKER_ML_PIDS[$s]:-}"
      [[ -z "$pid" ]] && continue   # was already running
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
  local s
  for ((s=0; s<${#WORKER_ML_PIDS[@]}; s++)); do
    local pid="${WORKER_ML_PIDS[$s]:-}"
    [[ -z "$pid" ]] && continue
    echo "[run_tests] Stopping ML slot=$s (pid $pid)…"
    kill -TERM "$pid" 2>/dev/null || true
  done
  # Give each service up to 10 s to finish saving its checkpoint.
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

# ----- Root aggregator lifecycle (federated mode only) ----------------------
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
  # Give it a second to start scanning the dir. Aggregator is lightweight —
  # no torch import — so it's ready almost immediately.
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

# ----- argument assembly -----------------------------------------------------
# Which flags each topology accepts. USA has the full set; abilene has the old
# ML flags + trafficMode; the rest are minimal.
topology_supports_flag() {
  local topo="$1" flag="$2"
  case "$topo" in
    usa) return 0 ;;  # all flags
    usa-fullrun)
      # usa-fullrun handles ML directly per-controller (mlPortBase, not
      # mlEndpoint) and exposes a subset of usa's knobs. It has no
      # trafficMode/tcp/failures/cripple — pings are full-mesh by default.
      case "$flag" in
        simTime|warmupS|seed|ml|mlPriority|mlIntervalS|mlActionScale|mlActionScaleStart|mlTaperTicks|mlExplore|mlResume|mlCheckpointEveryNTicks|backboneQueue|edgeQueue|sectionNodes|borderSwitches|interDomainRoutes|mlPortBase|pingIntervalS|pingCount) return 0 ;;
        *) return 1 ;;
      esac ;;
    abilene)
      case "$flag" in
        simTime|warmupS|trafficMode|seed|ml|mlIntervalS|mlActionScale|mlAlpha|mlBeta|mlGamma|mlResume|mlEndpoint) return 0 ;;
        *) return 1 ;;
      esac ;;
    mini-geant|two-switch-ping|stats-test)
      case "$flag" in simTime) return 0 ;; *) return 1 ;; esac ;;
    *) return 0 ;;
  esac
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
  add_bool_if_supported tcp      "$TCP"
  add_bool_if_supported failures "$FAILURES"
  add_bool_if_supported cripple  "$CRIPPLE"
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
  # $1 = pattern, $2 = file. Echoes the first numeric capture or empty.
  # Always returns 0 — a missing match is a normal case for partial logs.
  local pat="$1" file="$2"
  grep -E "$pat" "$file" 2>/dev/null | head -n1 | grep -oE '[0-9]+(\.[0-9]+)?' | head -n1 || true
}

# Pretty MJ / kW formatter.
fmt_J()  { awk -v v="${1:-0}" 'BEGIN{printf (v>=1e6)?"%.2f MJ":"%.0f J", (v>=1e6)?v/1e6:v}'; }
fmt_W()  { awk -v v="${1:-0}" 'BEGIN{printf (v>=1000)?"%.2f kW":"%.0f W", (v>=1000)?v/1000:v}'; }

summarize_log() {
  local f="$1" label="$2"
  # ---- QoS ----
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
  # ---- Energy ----
  local energy residual power per_sw_consumed per_sw_residual residual_frac
  energy=$(extract  '^[[:space:]]+Total consumed[[:space:]]+:' "$f")
  residual=$(extract '^[[:space:]]+Total residual[[:space:]]+:' "$f")
  power=$(extract   '^[[:space:]]+Total avg power[[:space:]]+:' "$f")
  per_sw_consumed=$(extract '^[[:space:]]+Per-switch consumed[[:space:]]+:' "$f")
  per_sw_residual=$(extract '^[[:space:]]+Per-switch residual[[:space:]]+:' "$f")
  residual_frac=$(extract   '^[[:space:]]+Residual fraction[[:space:]]+:' "$f")

  # Energy-per-delivered-bit (J/Mb): the headline efficiency metric.
  local jpermb=""
  if [[ -n "$energy" && -n "$rx" && "$rx" != "0" ]]; then
    # Assume ~1KB avg payload — same approximation across runs makes this
    # comparable; absolute number is not the point.
    jpermb=$(awk -v e="$energy" -v r="$rx" 'BEGIN{printf "%.3f", e/(r*1024*8/1e6)}')
  fi

  # ---- ML (aggregated from scratch/data/agent/metrics.csv) ----
  # aggregate_metrics.py prints KEY=value lines. Anything missing (e.g. when
  # the run was baseline-only) yields empty strings — that's the desired CSV
  # behaviour, not an error.
  local ml_ticks ml_reward_final ml_reward_mean ml_critic_loss ml_actor_loss
  ml_ticks=""; ml_reward_final=""; ml_reward_mean=""
  ml_critic_loss=""; ml_actor_loss=""
  if $ML && [[ -f "$CKPT_DIR/metrics.csv" ]]; then
    local agg
    agg=$(python3 "$SCRIPT_DIR/python/aggregate_metrics.py" \
            "$CKPT_DIR/metrics.csv" 2>/dev/null || true)
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

  # Append to CSV (created if absent). If the existing CSV has any other
  # header (old flat schema, partial archive, etc.), rotate it aside so we
  # don't mix schemas.
  local expected_header="timestamp,label,topology,sim_time_s,traffic_mode,seed,controller,priority,tcp,failures,cripple,ping_success_pct,rtt_avg_ms,rtt_jitter_ms,pdr_pct,e2e_delay_avg_ms,flows,tx_pkts,rx_pkts,hop_count_avg,energy_total_j,energy_residual_j,power_avg_w,per_sw_consumed_j,per_sw_residual_j,residual_pct,j_per_mb,ml_reward_final,ml_reward_mean_last25,ml_critic_loss_final,ml_actor_loss_final,ml_ticks"
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
    "$($TCP && echo 1 || echo 0)" "$($FAILURES && echo 1 || echo 0)" "$($CRIPPLE && echo 1 || echo 0)" \
    "${success:-}" "${rtt:-}" "${jitter:-}" "${delivery:-}" "${delay:-}" \
    "${flows:-}" "${tx:-}" "${rx:-}" "${hops:-}" \
    "${energy:-}" "${residual:-}" "${power:-}" \
    "${per_sw_consumed:-}" "${per_sw_residual:-}" "${residual_frac:-}" "${jpermb:-}" \
    "${ml_reward_final:-}" "${ml_reward_mean:-}" "${ml_critic_loss:-}" "${ml_actor_loss:-}" \
    "${ml_ticks:-}" \
    >>"$SUMMARY_CSV"
}

# ----- runner ----------------------------------------------------------------
ns3_log_env() {
  if $NS3_VERBOSE; then
    echo 'NS_LOG="ZmqOpenFlowController=level_info|prefix_time"'
  else
    echo ''
  fi
}

# Build once up front so parallel ./ns3 run --no-build invocations don't race
# on CMake/Ninja in the shared build/ dir. Concurrent cmake runs corrupt the
# build.ninja file and the slower-to-finish ones die.
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
  # Slot-aware ML endpoint injection. Only matters when --ml is on; harmless
  # otherwise (the binary ignores --mlEndpoint when ML is disabled).
  if $ML && topology_supports_flag "$TOPOLOGY" mlEndpoint; then
    NS3_ARGS+=("--mlEndpoint=$(ml_endpoint_for_slot "$slot")")
  fi
  # Federated-mode per-slot section args. cmd_federated populates the global
  # WORKER_SECTION_* arrays; an empty slot entry means "not sectioned".
  if [[ -n "${WORKER_SECTION_NODES[$slot]:-}" ]]; then
    NS3_ARGS+=("--sectionId=${WORKER_SECTION_IDS[$slot]}")
    NS3_ARGS+=("--sectionNodes=${WORKER_SECTION_NODES[$slot]}")
    # flashCrowdDst / blackHoleSwitch reference original (pre-filter) indices
    # that may not survive into this section. Use safe defaults: host 0 is
    # always present in any non-empty section after renumbering; switch 99
    # never matches anything, so the black-hole event becomes a no-op.
    NS3_ARGS+=("--flashCrowdDst=0")
    NS3_ARGS+=("--blackHoleSwitch=99")
  fi
  local logfile="$LOG_DIR/$(date +%Y%m%d-%H%M%S)-${label}.log"
  (( WORKERS > 1 )) && logfile="$LOG_DIR/$(date +%Y%m%d-%H%M%S)-${label}-w${slot}.log"
  local cmd="$TOPOLOGY ${NS3_ARGS[*]}"

  # Skip cmake/ninja per-invocation — we pre-built. Critical for parallel
  # safety: concurrent ./ns3 run calls corrupt build.ninja otherwise.
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

# Run jobs in parallel batches. Each job is a single string of bash that
# eval's. Caller passes job specs; we batch them into groups of $WORKERS
# and wait for the whole batch before starting the next. Each job gets
# its slot index as the first positional arg ($1).
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
cmd_single() {
  if $ML; then start_ml_service; fi
  local tag
  tag="$($ML && echo "ml-${PRIORITY}" || echo "baseline")"
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
    # Quote the closure so it runs in a subshell at dispatch time. $1 will
    # be the slot index supplied by dispatch_parallel.
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

# Curriculum-style training: cycle the agent through a variety of scenarios
# (traffic modes × failure conditions × seeds) keeping the same agent state
# throughout via --mlResume=true. The Python service stays running across
# all runs in this invocation, so replay + weights persist. Use this BEFORE
# `compare` / `eval` to get a well-trained agent.
#
# Override the curriculum with --trainRounds, --trainModes, --trainFailures,
# --trainCripple. Defaults give a broad but manageable training set:
#   rounds=2, modes={central,random,grouped}, failures={true,false}, cripple={true}
cmd_train() {
  ML=true
  EXPLORE=true
  RESUME=true
  start_ml_service
  local base_seed=$SEED
  local save_mode=$TRAFFIC_MODE save_fail=$FAILURES save_crip=$CRIPPLE
  local scenario_idx=0
  local total=0
  for r in $(seq 1 "$TRAIN_ROUNDS"); do
    for mode in $TRAIN_MODES; do
      for fail in $TRAIN_FAILURES; do
        for crip in $TRAIN_CRIPPLE; do
          TRAFFIC_MODE=$mode
          FAILURES=$fail
          CRIPPLE=$crip
          # SEED=$((base_seed + scenario_idx))
          SEED=$(od -An -N4 -tu4 < /dev/urandom | tr -d ' ')
          local label="train-r${r}-${mode}-fail${fail}-crip${crip}-seed${SEED}"
          echo
          echo "########## TRAINING ROUND ${r}/${TRAIN_ROUNDS} — scenario $((scenario_idx + 1)) ##########"
          run_one "$label"
          scenario_idx=$((scenario_idx + 1))
          total=$((total + 1))
        done
      done
    done
  done
  TRAFFIC_MODE=$save_mode; FAILURES=$save_fail; CRIPPLE=$save_crip; SEED=$base_seed
  echo
  echo "[run_tests] Training complete — $total scenarios across $TRAIN_ROUNDS rounds."
  echo "[run_tests] Run \`scratch/run_tests.sh eval ...\` or \`compare ...\` to evaluate."
}

## Federated mode: each section runs as its own ns-3 process with its own
## Local Controller and RL agent; the root aggregator FedAvgs their weights
## every $FEDAVG_EVERY training steps. The sections.json file partitions
## the topology; M = len(sections.json["sections"]).
cmd_federated() {
  : "${SECTIONS_JSON:=$SCRIPT_DIR/scenarios/$TOPOLOGY/sections.json}"
  if [[ ! -f "$SECTIONS_JSON" ]]; then
    echo "[run_tests] ERROR: sections.json not found at $SECTIONS_JSON" >&2
    echo "[run_tests] Provide --sectionsJson <path> or add one for topology '$TOPOLOGY'." >&2
    exit 1
  fi

  # Resolve M and the per-section node CSV via Python — avoids hand-rolling
  # JSON parsing in shell. Output is one section per line, "<id>\t<csv>".
  local sections_dump
  sections_dump=$(python3 - "$SECTIONS_JSON" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    spec = json.load(f)
for sec in spec["sections"]:
    nodes = ",".join(str(n) for n in sec["nodes"])
    print(f"{sec['id']}\t{nodes}")
PY
  )
  local num_sections=0
  WORKER_SECTION_IDS=()
  WORKER_SECTION_NODES=()
  while IFS=$'\t' read -r sid scsv; do
    [[ -z "$sid" ]] && continue
    WORKER_SECTION_IDS+=("$sid")
    WORKER_SECTION_NODES+=("$scsv")
    num_sections=$((num_sections + 1))
  done <<<"$sections_dump"

  if (( num_sections < 2 )); then
    echo "[run_tests] ERROR: federated mode needs >=2 sections; got $num_sections." >&2
    exit 1
  fi

  ML=true
  EXPLORE=true
  RESUME=true
  WORKERS=$num_sections
  FEDAVG_DIR="$SCRIPT_DIR/data/federated_weights"
  FEDAVG_AGG_LOG_DIR="$SCRIPT_DIR/data/fedavg"

  echo "[run_tests] Federated training: topology=$TOPOLOGY, sections=$num_sections, "
  echo "[run_tests]   fedavg_every=$FEDAVG_EVERY steps, timeout=$FEDAVG_TIMEOUT s,"
  echo "[run_tests]   sections.json=$SECTIONS_JSON"
  echo "[run_tests]   weights_dir=$FEDAVG_DIR"

  # Fresh state — leftover worker_*.pt or global_*.pt from a previous run
  # would confuse the aggregator's round-counter resume logic.
  mkdir -p "$FEDAVG_DIR" "$FEDAVG_AGG_LOG_DIR"
  rm -f "$FEDAVG_DIR"/*.pt "$FEDAVG_DIR"/*.pt.tmp 2>/dev/null || true

  start_aggregator
  start_ml_service

  local jobs=()
  local i
  for ((i=0; i<num_sections; i++)); do
    local sid="${WORKER_SECTION_IDS[$i]}"
    local label="${TOPOLOGY}-fed-s${sid}-seed${SEED}"
    # Section info lives in the WORKER_SECTION_* globals; run_one reads the
    # entry that matches its dispatch slot. No fragile per-job arg quoting.
    jobs+=("run_one \"$label\"")
  done
  dispatch_parallel "${jobs[@]}"

  echo
  echo "[run_tests] Federated run complete. Aggregator round log:"
  echo "[run_tests]   $FEDAVG_AGG_LOG_DIR/rounds.csv"
}

## Fullrun mode (Phase 2 of hierarchical SDN). One ns-3 process, the FULL
## topology, M Local Controllers, each owning its section. Inter-domain
## routing via static border-switch flow-mods (Option A) precomputed from
## sections.json's inter_domain_routes block. M ML services run on
## consecutive ports (mlPortBase + 0..M-1). FedAvg is opt-in via
## --fedAvgEverySteps; when on, the same root_aggregator.py from Phase 1
## FedAvgs the in-process controllers' weights via the shared dir.
cmd_fullrun() {
  : "${SECTIONS_JSON:=$SCRIPT_DIR/scenarios/usa/sections.json}"
  if [[ ! -f "$SECTIONS_JSON" ]]; then
    echo "[run_tests] ERROR: sections.json not found at $SECTIONS_JSON" >&2
    echo "[run_tests] Provide --sectionsJson <path>." >&2
    exit 1
  fi

  # Parse sections.json once. Output is three lines: section count, then
  # ';'-separated nodes CSVs, then ';'-separated border-switch CSVs, then
  # the inter_domain_routes "from:to:via:next,..." string.
  local parsed
  parsed=$(python3 - "$SECTIONS_JSON" <<'PY'
import json, sys
with open(sys.argv[1]) as f:
    spec = json.load(f)
sections = spec.get("sections", [])
nodes   = ";".join(",".join(str(n) for n in s["nodes"]) for s in sections)
borders = ";".join(",".join(str(n) for n in s.get("border_switches", [])) for s in sections)
idr = spec.get("inter_domain_routes", [])
idr_csv = ",".join(
    f"{r['from_section']}:{r['to_section']}:{r['via_switch']}:{r['next_switch']}"
    for r in idr
)
print(len(sections))
print(nodes)
print(borders)
print(idr_csv)
PY
  )
  local num_sections nodes_csv borders_csv idr_csv
  num_sections=$(echo "$parsed" | sed -n 1p)
  nodes_csv=$(echo   "$parsed" | sed -n 2p)
  borders_csv=$(echo "$parsed" | sed -n 3p)
  idr_csv=$(echo     "$parsed" | sed -n 4p)

  if (( num_sections < 2 )); then
    echo "[run_tests] ERROR: fullrun needs sections.json with >=2 sections; got $num_sections." >&2
    exit 1
  fi

  # In fullrun, M = number of in-process controllers AND number of ML
  # services. The scenario itself is single-process — WORKERS is reused
  # to size start_ml_service's per-controller port allocation.
  ML=true
  EXPLORE=true
  RESUME=true
  WORKERS=$num_sections
  TOPOLOGY="usa-fullrun"

  if (( FEDAVG_EVERY > 0 )); then
    FEDAVG_DIR="$SCRIPT_DIR/data/federated_weights"
    FEDAVG_AGG_LOG_DIR="$SCRIPT_DIR/data/fedavg"
    echo "[run_tests] Fullrun: topology=$TOPOLOGY sections=$num_sections fedavg=$FEDAVG_EVERY"
    mkdir -p "$FEDAVG_DIR" "$FEDAVG_AGG_LOG_DIR"
    rm -f "$FEDAVG_DIR"/*.pt "$FEDAVG_DIR"/*.pt.tmp 2>/dev/null || true
    start_aggregator
  else
    echo "[run_tests] Fullrun: topology=$TOPOLOGY sections=$num_sections fedavg=off"
  fi
  echo "[run_tests]   sections.json=$SECTIONS_JSON"

  ensure_built "$TOPOLOGY"
  start_ml_service

  # Section config + ML port base are passed directly via EXTRA_ARGS.
  # The scenario reads sectionNodes/borderSwitches/interDomainRoutes and
  # builds the multi-controller wiring; mlPortBase tells each controller
  # i to dial tcp://127.0.0.1:(mlPortBase+i).
  EXTRA_ARGS+=("--sectionNodes=$nodes_csv")
  EXTRA_ARGS+=("--borderSwitches=$borders_csv")
  EXTRA_ARGS+=("--interDomainRoutes=$idr_csv")
  EXTRA_ARGS+=("--mlPortBase=$ML_PORT_BASE")

  run_one "${TOPOLOGY}-seed${SEED}"

  if (( FEDAVG_EVERY > 0 )); then
    echo
    echo "[run_tests] Fullrun done. Aggregator round log:"
    echo "[run_tests]   $FEDAVG_AGG_LOG_DIR/rounds.csv"
  fi
}

cmd_summary() {
  local f="${1:-}"
  [[ -n "$f" && -f "$f" ]] || { echo "summary: file not found: $f" >&2; exit 1; }
  summarize_log "$f" "$(basename "$f" .log)"
}

cmd_clean() {
  echo "[run_tests] Removing $CKPT_DIR/*"
  rm -f "$CKPT_DIR"/*.pt "$CKPT_DIR"/*.pkl "$CKPT_DIR"/metrics.csv 2>/dev/null || true
  echo "[run_tests] Done. Next ML run starts fresh."
}

# ----- arg parser ------------------------------------------------------------
[[ $# -lt 1 ]] && { usage; exit 0; }
case "$1" in -h|--help) usage; exit 0 ;; esac
MODE="$1"; shift

# Summary takes a positional filename, not flags.
if [[ "$MODE" == "summary" ]]; then
  cmd_summary "${1:-}"
  exit $?
fi

if [[ "$MODE" == "clean" ]]; then
  cmd_clean
  exit 0
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
    --tcp)          TCP=true; shift ;;
    --no-tcp)       TCP=false; shift ;;
    --failures)     FAILURES=true; shift ;;
    --no-failures)  FAILURES=false; shift ;;
    --cripple)      CRIPPLE=true; shift ;;
    --no-cripple)   CRIPPLE=false; shift ;;
    --no-explore)   EXPLORE=false; shift ;;
    --no-resume)    RESUME=false; shift ;;
    --no-auto-ml)   AUTO_ML=false; shift ;;
    --verbose)      NS3_VERBOSE=true; shift ;;
    --workers)      WORKERS="$2"; shift 2 ;;
    --trainRounds)    TRAIN_ROUNDS="$2"; shift 2 ;;
    --trainModes)     TRAIN_MODES="$2"; shift 2 ;;
    --trainFailures)  TRAIN_FAILURES="$2"; shift 2 ;;
    --trainCripple)   TRAIN_CRIPPLE="$2"; shift 2 ;;
    --sectionsJson)       SECTIONS_JSON="$2"; shift 2 ;;
    --fedAvgEverySteps)   FEDAVG_EVERY="$2"; shift 2 ;;
    --fedAvgTimeoutS)     FEDAVG_TIMEOUT="$2"; shift 2 ;;
    -h|--help)      usage; exit 0 ;;
    --)             shift; EXTRA_ARGS=("$@"); break ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

ensure_built "$TOPOLOGY"

case "$MODE" in
  single)    cmd_single ;;
  compare)   cmd_compare ;;
  presets)   cmd_presets ;;
  seeds)     cmd_seeds ;;
  matrix)    cmd_matrix ;;
  eval)      cmd_eval ;;
  train)     cmd_train ;;
  federated) cmd_federated ;;
  fullrun)   cmd_fullrun ;;
  *) echo "Unknown mode: $MODE" >&2; usage; exit 1 ;;
esac

echo
echo "[run_tests] Done. Summary CSV: $SUMMARY_CSV"
