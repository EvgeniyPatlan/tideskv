#!/usr/bin/env bash
# TidesKV vs Kvrocks comparison benchmark (notes spec 2026-10-18).
# Usage: bench/compare/run.sh [--quick|--medium] [--smoke] [--relaxed-mode none|interval] [--dirty] [--out DIR]
#   --relaxed-mode defaults to none; --out defaults to build/bench/<UTC timestamp> and must not hold a non-empty data/
#   env (defaults): SERVER_CPUS 0-3, SERVER_MEM 8g, CLIENT_CPUS 8-11, THREADS 4, CLIENTS 50,
#     WARMUP/DURATION 60/180 (quick 10/30; medium as full), QUIESCE_MAX 600 (quick 60), all in seconds
# shellcheck source-path=SCRIPTDIR disable=SC2034  # ROOT, SIZE and DIRTY are read by lib.sh and the phases Task 4 adds
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
# shellcheck source=workloads.sh
source "$HERE/workloads.sh"
# shellcheck source=lib.sh
source "$HERE/lib.sh"

SIZE=full RELAXED=none DIRTY=0 SMOKE=0 OUT_ARG=""
while (($#)); do
  case $1 in
    --quick) SIZE=quick ;;
    --medium) SIZE=medium ;;
    --smoke) SMOKE=1 ;;
    --relaxed-mode) RELAXED=${2:?--relaxed-mode needs a value}; shift ;;
    --dirty) DIRTY=1 ;;
    --out) OUT_ARG=${2:?--out needs a value}; shift ;;
    -h|--help) sed -n 2,6p "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
[[ $RELAXED == none || $RELAXED == interval ]] || { echo "--relaxed-mode must be none or interval" >&2; exit 2; }

# Starts each server at both levels, checks parity and runs every workload's probe once (no memtier).
run_smoke() {
  local server level w line
  for server in tideskv kvrocks; do
    for level in relaxed strict; do
      start_server "$server" "$level" "$OUT/data/$server"
      parity_check "$server" "$level"
      for w in $WORKLOADS; do
        while read -r line; do
          # shellcheck disable=SC2086
          reply=$(cli $line)
          if [[ $reply == ERR* || $reply == WRONGTYPE* ]]; then log "probe $server $w [$line]: $reply"; return 1; fi
        done < <(wl_probe "$w")
      done
      stop_server
    done
  done
  log "smoke ok: $OUT"
}

if [[ $SIZE == quick ]]; then WARMUP=${WARMUP:-10} DURATION=${DURATION:-30} QUIESCE_MAX=${QUIESCE_MAX:-60}
else WARMUP=${WARMUP:-60} DURATION=${DURATION:-180} QUIESCE_MAX=${QUIESCE_MAX:-600}; fi
THREADS=${THREADS:-4}
CLIENTS=${CLIENTS:-50}

# Refuses when free disk is under 2 x (3 x logical) + 32 GiB (spec §8, raised after the quick run). DISK_FREE_OVERRIDE is a test hook.
disk_check() {
  local need free
  # Kvrocks measured ~8x end/logical in the quick run and its WAL retention is a fixed overhead: 3x per server, two servers, plus 32 GiB
  need=$(( $(wl_logical_bytes) * 3 * 2 + 32 * 1024 * 1024 * 1024 ))
  free=${DISK_FREE_OVERRIDE:-$(df --output=avail -B1 "$OUT" | tail -1)}
  if (( free < need )); then log "not enough disk: need $need bytes, $free free under $OUT"; return 1; fi
}

# "N logical CPUs on M physical cores" for SERVER_CPUS (on this host 0-3 is 2 cores with SMT).
server_cores() {
  local part c cpus=() parts
  IFS=, read -ra parts <<<"$SERVER_CPUS"
  for part in "${parts[@]}"; do
    if [[ $part == *-* ]]; then for ((c = ${part%-*}; c <= ${part#*-}; c++)); do cpus+=("$c"); done; else cpus+=("$part"); fi
  done
  echo "${#cpus[@]} logical CPUs on $(lscpu -p=CPU,CORE | awk -F, -v l=" ${cpus[*]} " \
    '!/^#/ && index(l, " " $1 " ") {print $2}' | sort -u | wc -l) physical cores"
}

# meta.json: one key=value argument per field, "#key=value" for integers.
write_meta() {
  local kv_digest sha
  kv_digest=$(docker image inspect -f '{{index .RepoDigests 0}}' "$KVROCKS_IMAGE" 2>/dev/null || echo "$KVROCKS_IMAGE")
  sha=$(git -C "$ROOT" rev-parse --short HEAD)
  if ((DIRTY)); then sha+=-dirty; fi
  python3 - "$OUT/meta.json" \
    "tideskv_sha=$sha" "kvrocks_image=$kv_digest" "tideskv_image=$TIDESKV_IMAGE" "memtier_image=$MEMTIER_IMAGE" \
    "size=$SIZE" "relaxed_mode=$RELAXED" "#warmup_s=$WARMUP" "#duration_s=$DURATION" \
    "server_cpus=$SERVER_CPUS" "server_cores=$(server_cores)" "server_mem=$SERVER_MEM" "#server_nofile=$SERVER_NOFILE" \
    "#threads=$THREADS" "#clients=$CLIENTS" "client_cpus=$CLIENT_CPUS" "memtier_run_flags=${MEMTIER_RUN_FLAGS[*]}" \
    "#w6_del_target_per_s=$W6_DEL_RATE" "#w6_del_clients=$W6_DEL_CLIENTS" \
    "#quiesce_window_s=$QUIESCE_WINDOW" "#quiesce_cpu_pct=$QUIESCE_CPU_PCT" \
    "#quiesce_write_bytes=$QUIESCE_WRITE_BYTES" "#quiesce_max_s=$QUIESCE_MAX" \
    "host=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs), kernel $(uname -r)" "started_utc=$TS" <<'EOF'
import json, sys
meta = {}
for arg in sys.argv[2:]:
    key, value = arg.split("=", 1)
    meta[key.lstrip("#")] = int(value) if key.startswith("#") else value
with open(sys.argv[1], "w") as f:
    json.dump(meta, f, indent=2)
EOF
}

# wl_load supplies its own --threads/--clients (ruling T2-R1)
# shellcheck disable=SC2317  # callbacks are invoked indirectly by wl_load / wl_run
_load_cb() { memtier "$CUR/load/$LOAD_NAME-$LOAD_N" "$@" || return; LOAD_N=$((LOAD_N + 1)); }

load_all() {
  local ds start line want got i
  start=$(date +%s)
  for ds in $DATASETS; do
    LOAD_NAME=$ds LOAD_N=1
    log "loading $ds"
    wl_load "$ds" _load_cb || { log "load of $ds failed; see $CUR/load/"; return 1; }
  done
  # Kvrocks' DBSIZE is a cache that stays 0 until a DBSIZE SCAN has run; poll only that line for the scan to land.
  local tries
  if [[ $SERVER_NAME == kvrocks ]]; then cli DBSIZE SCAN >/dev/null; fi
  while read -r want line; do
    tries=1
    if [[ $SERVER_NAME == kvrocks && $line == DBSIZE ]]; then tries=900; fi
    for ((i = 0; i < tries; i++)); do
      # shellcheck disable=SC2086
      got=$(cli $line)
      [[ $got =~ ^[0-9]+$ ]] && (( got >= want )) && break
      if ((i + 1 < tries)); then sleep 2; fi
    done
    if ! [[ $got =~ ^[0-9]+$ ]] || (( got < want )); then log "load check failed: $line = [$got], want >= $want"; return 1; fi
  done < <(wl_verify_cmds)
  LOAD_SECONDS=$(( $(date +%s) - start ))
}

# Waits for the server to go quiet and records the wait in $1.
settle() {
  local r
  # in the background so that a TERM or INT is handled now: bash runs a trap only once the foreground
  # command returns, and a wait can last QUIESCE_MAX
  quiesce >"$1" & wait $! || { log "cannot read $SERVER_CONTAINER's cgroup ($SERVER_CGROUP)"; return 1; }
  r=$(<"$1")
  log "$SERVER_NAME $(settle_text "$r")"
}

# "settled after 12s", or "did not settle within 600s" for a capped wait
settle_text() {
  if [[ $1 == *capped ]]; then echo "did not settle within ${1%% *}s"; else echo "settled after ${1}s"; fi
}

# Stops the docker stats loop and any sample still in flight, so no post-run sample is recorded.
stop_sampler() {
  kill -STOP "$1" 2>/dev/null || return 0
  pkill -P "$1" 2>/dev/null || true
  kill "$1" 2>/dev/null || true
  kill -CONT "$1" 2>/dev/null || true
  wait "$1" 2>/dev/null || true
}

# shellcheck disable=SC2317
_run_cb() { RUN_ARGS=("$@"); }
# shellcheck disable=SC2317
_del_cb() { DEL_ARGS=("$@"); }

# Warm-up then the timed run of one workload; records .json, .stats.jsonl, .status.
run_workload() {
  local level=$1 w=$2 base="$CUR/$1/$2" line reply stats_pid del_pid=""
  mkdir -p "$OUT/$CUR/$level"
  while read -r line; do
    # shellcheck disable=SC2086
    reply=$(cli $line 2>&1)
    if [[ $reply == ERR* || $reply == WRONGTYPE* ]]; then echo "probe: $reply" >"$OUT/$base.status"; return; fi
  done < <(wl_probe "$w")
  settle "$OUT/$base.settle" || { echo "could not read the server's cgroup to wait for it to settle" >"$OUT/$base.status"; return; }
  wl_run "$w" _run_cb
  local common=(--threads="$THREADS" --clients="$CLIENTS" --pipeline=1 "${MEMTIER_RUN_FLAGS[@]}")
  memtier "$base.warmup" "${common[@]}" --test-time="$WARMUP" "${RUN_ARGS[@]}" || true
  if [[ $w == W6 ]]; then
    wl_run_w6_del _del_cb
    memtier "$CUR/$level/W6-del" --test-time="$DURATION" "${DEL_ARGS[@]}" & del_pid=$!
  fi
  # docker stats gives peak memory; CPU is the cgroup's usage_usec delta over exactly the timed run
  ( while kill -0 $$ 2>/dev/null; do docker stats --no-stream --format '{{json .}}' "$SERVER_CONTAINER"; sleep 2; done ) \
    >"$OUT/$base.stats.jsonl" 2>/dev/null & stats_pid=$!
  local cpu0 cpu1
  cpu0=$(cg_sample) || cpu0=""
  if memtier "$base" "${common[@]}" --test-time="$DURATION" "${RUN_ARGS[@]}"; then
    echo ok >"$OUT/$base.status"
  else
    echo "memtier exited $?; see $base.log" >"$OUT/$base.status"
  fi
  cpu1=$(cg_sample) || cpu1=""
  if [[ -n $cpu0 && -n $cpu1 ]]; then echo $(( ${cpu1%% *} - ${cpu0%% *} )) >"$OUT/$base.cpu"; fi
  stop_sampler "$stats_pid"
  if [[ -n $del_pid ]] && ! wait "$del_pid"; then
    echo "W6 DEL memtier failed; see $CUR/$level/W6-del.log" >>"$OUT/$base.status"
    if [[ $(head -1 "$OUT/$base.status") == ok ]]; then sed -i 1d "$OUT/$base.status"; fi
  fi
  if [[ $(docker inspect -f '{{.State.Running}}' "$SERVER_CONTAINER" 2>/dev/null) != true ]]; then
    echo "server stopped during the run (OOM-killed: $(docker inspect -f '{{.State.OOMKilled}}' "$SERVER_CONTAINER"))" >"$OUT/$base.status"
    return 1
  fi
}

_du() { du -sb "$1" | cut -f1; }

# Records why a server's run failed, then stops it so it cannot keep the server cores during the other run.
_server_failed() {
  echo "$1" >"$OUT/$CUR/run.failed"
  log "$CUR: $1"
  stop_server
}

run_server() {
  local server=$1 level w after
  CUR=$server
  local data="$OUT/data/$server"
  start_server "$server" relaxed "$data" || { _server_failed "server did not start (relaxed)"; return 1; }
  parity_check "$server" relaxed || { _server_failed "parity check failed (relaxed); see $server/config.txt"; return 1; }
  load_all || { _server_failed "load failed or the load check did not pass; see $server/load/"; return 1; }
  settle "$OUT/$server/settle-load" || { _server_failed "could not read the server's cgroup"; return 1; }
  after=$(_du "$data")
  # relaxed and strict deliberately share one data dir (spec §5 step 4)
  for level in relaxed strict; do
    if [[ $level == strict ]]; then
      stop_server
      start_server "$server" strict "$data" || { _server_failed "server did not start (strict)"; return 1; }
      parity_check "$server" strict || { _server_failed "parity check failed (strict); see $server/config.txt"; return 1; }
      settle "$OUT/$server/settle-restart" || { _server_failed "could not read the server's cgroup"; return 1; }
    fi
    for w in $WORKLOADS; do
      log "$server $level $w"
      run_workload "$level" "$w" || { log "$server died; skipping its remaining workloads"; break 2; }
    done
  done
  stop_server
  printf '{"after_load_bytes": %s, "end_bytes": %s, "load_seconds": %s, "logical_bytes": %s}\n' \
    "$after" "$(_du "$data")" "$LOAD_SECONDS" "$(wl_logical_bytes)" >"$OUT/$server/disk.json"
}

# The servers write the data dirs as other uids, so the host user cannot delete them: empty them from a container.
remove_data() {
  [[ -d $OUT/data ]] || return 0
  docker run --rm --name "tideskv-bench-clean-$TS" -v "$OUT/data:/d" --entrypoint find "$CLI_IMAGE" \
    /d -mindepth 1 -delete >/dev/null 2>&1 || log "could not empty $OUT/data"
}

run_full() {
  wl_sizes "$SIZE"
  disk_check
  docker pull -q "$KVROCKS_IMAGE" >/dev/null; docker pull -q "$MEMTIER_IMAGE" >/dev/null; docker pull -q "$CLI_IMAGE" >/dev/null
  write_meta
  local server
  for server in tideskv kvrocks; do run_server "$server" || log "$server: run failed, continuing"; done
  remove_data
  local rc=0
  python3 "$HERE/report.py" "$OUT" || rc=$?
  log "report: $OUT/report.md"
  exit $rc
}

# sourcing run.sh (the tests do) defines the helpers without running anything
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  bench_init "$OUT_ARG"
  build_tideskv_image
  if ((SMOKE)); then run_smoke; exit; fi
  run_full
fi
