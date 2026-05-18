#!/usr/bin/env bash
# scratch/run_tests.sh — drive ns-3 SDN test variants.
#
# Modes:
#   single    one run with whatever flags you pass
#   compare   baseline + one ML run, same seed/params
#   presets   baseline + balanced + delay_first + energy_first ML runs
#   seeds     repeat the same config across N consecutive seeds
#   matrix    full statistical matrix (seeds × trafficMode × failures, ml vs baseline)
#   eval      pretrained eval-only run (--mlExplore=false --mlResume=true)
#   summary   parse a saved log and print headline numbers
#   clean     wipe checkpoints + replay buffer (forces fresh learning)
#
# Each ns3 run is teed to scratch/data/results/logs/ with a timestamped name,
# then summarized to scratch/data/results/summary.csv.
#
# Examples:
#   scratch/run_tests.sh compare --simTime 600 --trafficMode central --udp --failures --cripple
#   scratch/run_tests.sh presets --simTime 600 --udp --failures --cripple
#   scratch/run_tests.sh seeds --seeds 5 --ml --priority balanced --simTime 1200 --udp --failures --cripple
#   scratch/run_tests.sh matrix --seeds 3 --simTime 600 --priority balanced
#   scratch/run_tests.sh eval --simTime 200 --priority balanced --udp --failures --cripple
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
UDP=false
FAILURES=false
CRIPPLE=false
EXPLORE=true
RESUME=true
N_SEEDS=5
EVAL_WINDOW=0
EXTRA_ARGS=()
AUTO_ML=true
NS3_VERBOSE=false

# `train` mode curriculum — overrideable via flags.
TRAIN_ROUNDS=2
TRAIN_MODES="central random grouped"
TRAIN_FAILURES="true false"
TRAIN_CRIPPLE="true"

ML_PID=""
WE_STARTED_ML=false

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
  summary FILE          Print headline stats from a saved log file
  clean                 Remove ML checkpoint + replay buffer

Options:
  --topology X          usa | abilene | mini-geant | two-switch-ping  (default: usa)
  --simTime N           Simulation duration in seconds                 (default: 600)
  --warmupS N           Warmup window                                  (default: 10)
  --trafficMode X       random | central | grouped                     (default: central)
  --seed N              Base random seed                               (default: 12345)
  --seeds N             Number of seeds for 'seeds' / 'matrix' modes   (default: 5)
  --priority X          balanced | delay_first | energy_first | custom (default: balanced)
  --ml | --no-ml        Enable / disable the ML controller             (default: off)
  --udp | --no-udp      Toggle UDP background load                     (default: off)
  --failures | --no-failures   Toggle scheduled link churn             (default: off)
  --cripple  | --no-cripple    Toggle Missoula crippling (USA only)    (default: off)
  --no-explore          Disable OU noise & gradient updates (eval mode)
  --no-resume           Don't resume the ML agent from checkpoint
  --evalWindowS N       Delay FlowMonitor reset by N seconds past warmup
  --no-auto-ml          Don't auto-start the Python ML service
  --verbose             Pass NS_LOG to surface controller info

Train-mode curriculum (defaults give 12 scenarios × 2 rounds = 24 runs):
  --trainRounds N       How many times to loop the whole curriculum (default 2)
  --trainModes "..."    Space-separated traffic modes (default: "central random grouped")
  --trainFailures "..." Space-separated bool values  (default: "true false")
  --trainCripple "..."  Space-separated bool values  (default: "true")

  -- arg1 arg2 ...      Forward extra args verbatim to ns3 (after --)

Where logs go:   scratch/data/results/logs/
Summary CSV:     scratch/data/results/summary.csv
EOF
}

# ----- ML service lifecycle --------------------------------------------------
ml_service_running() {
  # Probe by attempting a TCP connect — more reliable than parsing `ss`/`lsof`
  # output, which varies wildly across containers.
  python3 - <<'PY' 2>/dev/null
import socket, sys
s = socket.socket()
s.settimeout(0.4)
try:
    s.connect(("127.0.0.1", 5555))
    s.close()
    sys.exit(0)
except OSError:
    sys.exit(1)
PY
}

start_ml_service() {
  $AUTO_ML || return 0
  if ml_service_running; then
    echo "[run_tests] ML service already running on :5555 — reusing."
    return 0
  fi
  echo "[run_tests] Starting ML service in background…"
  (
    cd "$NS3_ROOT"
    exec python3 -u "$ML_SERVICE_PY"
  ) >"$LOG_DIR/ml-service.log" 2>&1 &
  ML_PID=$!
  WE_STARTED_ML=true
  # First-run torch import can take >10s. Wait up to 30s, polling once a
  # second, but bail early if the python process dies.
  local elapsed=0 max=30
  while (( elapsed < max )); do
    if ! kill -0 "$ML_PID" 2>/dev/null; then
      echo "[run_tests] ERROR: ML service died during startup. Last log lines:" >&2
      tail -20 "$LOG_DIR/ml-service.log" >&2 || true
      WE_STARTED_ML=false
      ML_PID=""
      return 1
    fi
    if ml_service_running; then
      echo "[run_tests] ML service ready (pid $ML_PID, took ~${elapsed}s)."
      return 0
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done
  echo "[run_tests] WARN: ML service did not bind :5555 within ${max}s. Last log lines:" >&2
  tail -10 "$LOG_DIR/ml-service.log" >&2 || true
}

stop_ml_service() {
  if $WE_STARTED_ML && [[ -n "$ML_PID" ]]; then
    echo "[run_tests] Stopping ML service (pid $ML_PID)…"
    # SIGTERM gives the service a chance to save its checkpoint before dying.
    kill -TERM "$ML_PID" 2>/dev/null || true
    # Give it up to 10 s to finish the save.
    local waited=0
    while (( waited < 20 )) && kill -0 "$ML_PID" 2>/dev/null; do
      sleep 0.5
      waited=$((waited + 1))
    done
    kill -KILL "$ML_PID" 2>/dev/null || true
    wait "$ML_PID" 2>/dev/null || true
    WE_STARTED_ML=false
    ML_PID=""
  fi
}
trap stop_ml_service EXIT INT TERM

# ----- argument assembly -----------------------------------------------------
# Which flags each topology accepts. USA has the full set; abilene has the old
# ML flags + trafficMode; the rest are minimal.
topology_supports_flag() {
  local topo="$1" flag="$2"
  case "$topo" in
    usa) return 0 ;;  # all flags
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
  add_bool_if_supported udp      "$UDP"
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
  local success loss rtt delivery delay flows tx rx energy power
  local residual per_sw_consumed per_sw_residual residual_frac
  success=$(extract '^[[:space:]]+Success[[:space:]]+:' "$f")
  loss=$(extract    '^[[:space:]]+Loss[[:space:]]+:' "$f")
  rtt=$(extract     '^[[:space:]]+Avg RTT[[:space:]]+:' "$f")
  flows=$(extract   '^[[:space:]]+Flows[[:space:]]+:' "$f")
  tx=$(extract      '^[[:space:]]+Tx packets[[:space:]]+:' "$f")
  rx=$(extract      '^[[:space:]]+Rx packets[[:space:]]+:' "$f")
  delivery=$(extract '^[[:space:]]+Delivery[[:space:]]+:' "$f")
  delay=$(extract    '^[[:space:]]+Avg delay[[:space:]]+:' "$f")
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

  echo "── $label ──"
  printf "  ping success : %s%%\n"   "${success:-—}"
  printf "  avg RTT      : %s ms\n"  "${rtt:-—}"
  printf "  delivery     : %s%%\n"   "${delivery:-—}"
  printf "  avg delay    : %s ms\n"  "${delay:-—}"
  printf "  total energy : %s  (avg %s)\n" "$(fmt_J "${energy:-0}")" "$(fmt_W "${power:-0}")"
  [[ -n "$per_sw_consumed" ]] && printf "  per-switch   : consumed %s, left %s (%s%%)\n" \
    "$(fmt_J "${per_sw_consumed:-0}")" "$(fmt_J "${per_sw_residual:-0}")" "${residual_frac:-—}"
  [[ -n "$jpermb" ]] && printf "  J / Mb delivered : %s\n" "$jpermb"
  echo

  # Append to CSV (created if absent). If the existing CSV has the older,
  # shorter header, rotate it aside so we don't mix schemas.
  local expected_header="timestamp,label,topology,simTime,trafficMode,seed,ml,priority,udp,failures,cripple,success,rtt_ms,delivery,delay_ms,flows,tx,rx,energy_J,residual_J,power_W,per_sw_consumed_J,per_sw_residual_J,residual_pct,j_per_Mb"
  if [[ -f "$SUMMARY_CSV" ]] && [[ "$(head -n1 "$SUMMARY_CSV")" != "$expected_header" ]]; then
    local archived="${SUMMARY_CSV%.csv}.$(date +%Y%m%d-%H%M%S).csv"
    mv "$SUMMARY_CSV" "$archived"
    echo "[run_tests] CSV schema changed — archived old rows to $archived"
  fi
  if [[ ! -f "$SUMMARY_CSV" ]]; then
    echo "$expected_header" >"$SUMMARY_CSV"
  fi
  printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
    "$(date -Iseconds)" "$label" "$TOPOLOGY" "$SIM_TIME" "$TRAFFIC_MODE" "$SEED" \
    "$($ML && echo ml || echo baseline)" "$($ML && echo "$PRIORITY" || echo -)" \
    "$($UDP && echo 1 || echo 0)" "$($FAILURES && echo 1 || echo 0)" "$($CRIPPLE && echo 1 || echo 0)" \
    "${success:-}" "${rtt:-}" "${delivery:-}" "${delay:-}" \
    "${flows:-}" "${tx:-}" "${rx:-}" "${energy:-}" "${residual:-}" "${power:-}" \
    "${per_sw_consumed:-}" "${per_sw_residual:-}" "${residual_frac:-}" "${jpermb:-}" \
    >>"$SUMMARY_CSV"
}

# ----- runner ----------------------------------------------------------------
ns3_log_env() {
  if $NS3_VERBOSE; then
    echo 'NS_LOG="ZmqOpenFlowController=info"'
  else
    echo ''
  fi
}

run_one() {
  local label="$1"
  build_args
  local logfile="$LOG_DIR/$(date +%Y%m%d-%H%M%S)-${label}.log"
  local cmd="$TOPOLOGY ${NS3_ARGS[*]}"

  echo "════════════════════════════════════════════════════════════════"
  echo "[run_tests] $label"
  echo "[run_tests] ./ns3 run \"$cmd\""
  echo "[run_tests] → $logfile"
  echo "════════════════════════════════════════════════════════════════"

  (
    cd "$NS3_ROOT"
    if $NS3_VERBOSE; then
      NS_LOG="ZmqOpenFlowController=info" ./ns3 run "$cmd"
    else
      ./ns3 run "$cmd"
    fi
  ) 2>&1 | tee "$logfile"

  summarize_log "$logfile" "$label"
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
  for i in $(seq 0 $((N_SEEDS - 1))); do
    SEED=$((base + i))
    local tag
    tag="$($ML && echo "ml-${PRIORITY}" || echo "baseline")"
    run_one "${TOPOLOGY}-${tag}-${TRAFFIC_MODE}-seed${SEED}"
  done
  SEED=$base
}

cmd_matrix() {
  start_ml_service
  local base_seed=$SEED save_mode=$TRAFFIC_MODE save_fail=$FAILURES save_ml=$ML
  for mode in central random grouped; do
    for fail in true false; do
      TRAFFIC_MODE=$mode
      FAILURES=$fail
      for i in $(seq 0 $((N_SEEDS - 1))); do
        SEED=$((base_seed + i))
        ML=false; run_one "${TOPOLOGY}-baseline-${mode}-fail${fail}-seed${SEED}"
        ML=true;  run_one "${TOPOLOGY}-ml-${PRIORITY}-${mode}-fail${fail}-seed${SEED}"
      done
    done
  done
  SEED=$base_seed; TRAFFIC_MODE=$save_mode; FAILURES=$save_fail; ML=$save_ml
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
          SEED=$((base_seed + scenario_idx))
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
    --udp)          UDP=true; shift ;;
    --no-udp)       UDP=false; shift ;;
    --failures)     FAILURES=true; shift ;;
    --no-failures)  FAILURES=false; shift ;;
    --cripple)      CRIPPLE=true; shift ;;
    --no-cripple)   CRIPPLE=false; shift ;;
    --no-explore)   EXPLORE=false; shift ;;
    --no-resume)    RESUME=false; shift ;;
    --no-auto-ml)   AUTO_ML=false; shift ;;
    --verbose)      NS3_VERBOSE=true; shift ;;
    --trainRounds)    TRAIN_ROUNDS="$2"; shift 2 ;;
    --trainModes)     TRAIN_MODES="$2"; shift 2 ;;
    --trainFailures)  TRAIN_FAILURES="$2"; shift 2 ;;
    --trainCripple)   TRAIN_CRIPPLE="$2"; shift 2 ;;
    -h|--help)      usage; exit 0 ;;
    --)             shift; EXTRA_ARGS=("$@"); break ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

case "$MODE" in
  single)  cmd_single ;;
  compare) cmd_compare ;;
  presets) cmd_presets ;;
  seeds)   cmd_seeds ;;
  matrix)  cmd_matrix ;;
  eval)    cmd_eval ;;
  train)   cmd_train ;;
  *) echo "Unknown mode: $MODE" >&2; usage; exit 1 ;;
esac

echo
echo "[run_tests] Done. Summary CSV: $SUMMARY_CSV"
