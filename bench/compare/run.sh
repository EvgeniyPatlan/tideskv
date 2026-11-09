#!/usr/bin/env bash
# TidesKV vs Kvrocks comparison benchmark (notes spec 2026-10-18).
# Usage: bench/compare/run.sh [--quick|--medium|--large [--no-w7-sweep]] [--smoke] [--relaxed-mode none|interval] [--dirty] [--out DIR]
#   --relaxed-mode defaults to none; --out defaults to build/bench/<UTC timestamp> and must not hold a non-empty data/
#   --large runs W7 (256 KiB values) only; with --smoke on 512 keys and 10/30 s phases
#   env (defaults): SERVER_CPUS 0-3, SERVER_MEM 8g (large 4g), CLIENT_CPUS 8-11, THREADS 4, CLIENTS 50,
#     WARMUP/DURATION 60/180 (quick 10/30; medium as full), QUIESCE_MAX 600 (quick 60), all in seconds
# shellcheck source-path=SCRIPTDIR disable=SC2034  # ROOT, SIZE and DIRTY are read by lib.sh and the phases Task 4 adds
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# lib.sh defaults SERVER_MEM to 8g; --large defaults to 4g unless the caller set it
SERVER_MEM_ENV=${SERVER_MEM:-}
ROOT=$(cd "$HERE/../.." && pwd)
# shellcheck source=workloads.sh
source "$HERE/workloads.sh"
# shellcheck source=lib.sh
source "$HERE/lib.sh"

SIZE=full RELAXED=none DIRTY=0 SMOKE=0 OUT_ARG="" W7_SWEEP=1
while (($#)); do
  case $1 in
    --quick) SIZE=quick ;;
    --medium) SIZE=medium ;;
    --large) SIZE=large ;;
    --no-w7-sweep) W7_SWEEP=0 ;;
    --smoke) SMOKE=1 ;;
    --relaxed-mode) RELAXED=${2:?--relaxed-mode needs a value}; shift ;;
    --dirty) DIRTY=1 ;;
    --out) OUT_ARG=${2:?--out needs a value}; shift ;;
    -h|--help) sed -n 2,7p "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
[[ $RELAXED == none || $RELAXED == interval ]] || { echo "--relaxed-mode must be none or interval" >&2; exit 2; }
# --large --smoke is a real W7 run on a tiny dataset, not the start-and-probe smoke
if [[ $SIZE == large ]] && ((SMOKE)); then SIZE=large-smoke SMOKE=0; fi
if ((!W7_SWEEP)) && [[ $SIZE != large* ]]; then echo "--no-w7-sweep needs --large" >&2; exit 2; fi

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

if [[ $SIZE == quick || $SIZE == large-smoke ]]; then WARMUP=${WARMUP:-10} DURATION=${DURATION:-30} QUIESCE_MAX=${QUIESCE_MAX:-60}
else WARMUP=${WARMUP:-60} DURATION=${DURATION:-180} QUIESCE_MAX=${QUIESCE_MAX:-600}; fi
THREADS=${THREADS:-4}
CLIENTS=${CLIENTS:-50}

# Refuses when free disk is under 2 x (3 x logical) + 32 GiB (spec §8, raised after the quick run). DISK_FREE_OVERRIDE is a test hook.
disk_check() {
  local need free
  # Kvrocks measured ~8x end/logical in the quick run and its WAL retention is a fixed overhead: 3x for the one server
  # whose data is on disk at a time (run_full empties it before the next), plus 32 GiB
  need=$(( $(wl_logical_bytes) * 3 + 32 * 1024 * 1024 * 1024 ))
  # W7 holds one server's data at a time: 2x logical covers the load plus M32's rewrites, plus 8 GiB and fio's file
  # (80 GiB at large with its 8 GiB fio file)
  if [[ $SIZE == large* ]]; then
    need=$(( $(wl_logical_bytes) * 2 + 8 * 1024 * 1024 * 1024 + $(size_bytes "${FIO_SIZE:-0}") ))
  fi
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

# meta.json: one key=value argument per field, "#key=value" for integers; META_EXTRA adds fields (W7's).
META_EXTRA=()
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
    "host=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs), kernel $(uname -r)" "started_utc=$TS" \
    "${META_EXTRA[@]}" <<'EOF'
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
  if [[ $SERVER_NAME == kvrocks* ]]; then cli DBSIZE SCAN >/dev/null; fi
  while read -r want line; do
    tries=1
    if [[ $SERVER_NAME == kvrocks* && $line == DBSIZE ]]; then tries=900; fi
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
  for server in tideskv kvrocks; do
    run_server "$server" || log "$server: run failed, continuing"
    remove_data  # one server's data at a time; disk.json already holds its sizes
  done
  local rc=0
  python3 "$HERE/report.py" "$OUT" || rc=$?
  log "report: $OUT/report.md"
  exit $rc
}

# ---- W7 (--large): 256 KiB values on a miss-heavy cache (notes design 2026-11-09) ----
W7_SERVERS="tideskv kvrocks kvrocks-blob"
CLIENT_BOUND_PCT=85

# The phases in run order. The workers-16 pass at R32 runs right after R32, on the same data, before M32 rewrites it.
w7_phase_list() {
  local p
  for p in $W7_PHASES; do
    echo "$p"
    if [[ $p == R32 ]] && ((W7_SWEEP)); then echo R32w16; fi
  done
}

# Stops a background sampler started with "&" (same as stop_sampler; separate name for the W7 ones).
_stop_bg() { local p; for p in "$@"; do stop_sampler "$p"; done; }

# One W7 phase on a freshly restarted server: settle, warm-up, then the timed run with every W7 measurement.
# Writes <server>/large/<phase>.{json,status,settle,cpu,io0,io1,io1s,io2,settle-after,info0,info1,stats.jsonl,
# client.jsonl,diskstats}. A status of "ok" can carry warning lines after it (a failed page-cache drop).
run_w7_phase() {
  local phase=$1 base="$CUR/large/$1" mc="tideskv-bench-memtier-$TS-w7-$CUR-$1" spid dpid cpid cpu0 cpu1 ds
  mkdir -p "$OUT/$CUR/large"
  ds=$OUT/$base.diskstats
  settle "$OUT/$base.settle" || { echo "could not read the server's cgroup to wait for it to settle" >"$OUT/$base.status"; return; }
  wl_run_w7 "$phase" _run_cb || return
  memtier "$base.warmup" --test-time="$WARMUP" "${RUN_ARGS[@]}" || true
  cli INFO >"$OUT/$base.info0" 2>&1 || true
  # the warm-up's dirty pages are written back now, not charged to the timed window
  sync
  stats_sampler "$SERVER_CONTAINER" >"$OUT/$base.stats.jsonl" & spid=$!
  stats_sampler "$mc" >"$OUT/$base.client.jsonl" & cpid=$!
  : >"$ds"
  diskstats_sampler >>"$ds" & dpid=$!
  cg_io_snapshot >"$OUT/$base.io0"
  cpu0=$(cg_sample) || cpu0=""
  # aqu-sz and r_await are computed between these markers only
  printf 'M start %s\n' "$(date +%s%3N)" >>"$ds"
  if MEMTIER_CNAME=$mc memtier "$base" --test-time="$DURATION" "${RUN_ARGS[@]}"; then
    echo ok >"$OUT/$base.status"
  else
    echo "memtier exited $?; see $base.log" >"$OUT/$base.status"
  fi
  printf 'M end %s\n' "$(date +%s%3N)" >>"$ds"
  cpu1=$(cg_sample) || cpu1=""
  cg_io_snapshot >"$OUT/$base.io1"
  # write amp also counts the window's dirty pages, which a relaxed server writes back after it (charged to its cgroup)
  sync
  cg_io_snapshot >"$OUT/$base.io1s"
  _stop_bg "$spid" "$cpid" "$dpid"
  if [[ -n $cpu0 && -n $cpu1 ]]; then echo $(( ${cpu1%% *} - ${cpu0%% *} )) >"$OUT/$base.cpu"; fi
  cli INFO >"$OUT/$base.info1" 2>&1 || true
  if ((EVICT_FAILED)); then echo "warning: the data dir could not be dropped from the page cache" >>"$OUT/$base.status"; fi
  # and the flush and compaction debt the window left: "write amp incl. settle"
  settle "$OUT/$base.settle-after" || true
  cg_io_snapshot >"$OUT/$base.io2"
  if [[ $(docker inspect -f '{{.State.Running}}' "$SERVER_CONTAINER" 2>/dev/null) != true ]]; then
    echo "server stopped during the run (OOM-killed: $(docker inspect -f '{{.State.OOMKilled}}' "$SERVER_CONTAINER"))" >"$OUT/$base.status"
    return 1
  fi
}

# Load once, then for each phase: restart (start_server drops the data dir from the page cache) and run it.
run_server_large() {
  local server=$1 phase after data
  data="$OUT/data/$server" CUR=$server SERVER_WORKERS=4
  start_server "$server" relaxed "$data" || { _server_failed "server did not start"; return 1; }
  parity_check "$server" relaxed load || { _server_failed "parity check failed; see $server/config.txt"; return 1; }
  load_all || { _server_failed "load failed or the load check did not pass; see $server/load/"; return 1; }
  settle "$OUT/$server/settle-load" || { _server_failed "could not read the server's cgroup"; return 1; }
  after=$(_du "$data")
  for phase in $(w7_phase_list); do
    SERVER_WORKERS=4
    if [[ $phase == *w16 ]]; then SERVER_WORKERS=16; fi
    stop_server
    start_server "$server" relaxed "$data" || { _server_failed "server did not start ($phase)"; return 1; }
    parity_check "$server" relaxed "$phase" || { _server_failed "parity check failed ($phase); see $server/config.txt"; return 1; }
    log "$server W7 $phase"
    run_w7_phase "$phase" || { log "$server died; skipping its remaining phases"; break; }
  done
  stop_server
  printf '{"after_load_bytes": %s, "end_bytes": %s, "load_seconds": %s, "logical_bytes": %s}\n' \
    "$after" "$(_du "$data")" "$LOAD_SECONDS" "$(wl_logical_bytes)" >"$OUT/$server/disk.json"
}

run_large() {
  wl_sizes "$SIZE"
  SERVER_MEM=${SERVER_MEM_ENV:-4g} BLOCK_CACHE_MB=1024 THREADS=$W7_THREADS CLIENTS=$W7_CLIENTS W7_LARGE=1 COMPRESSION=no
  FIO_QDS=${FIO_QDS:-1 2 4 8 16 32}
  if [[ $SIZE == large ]]; then FIO_RUNTIME=${FIO_RUNTIME:-30} FIO_SIZE=${FIO_SIZE:-8g}
  else FIO_RUNTIME=${FIO_RUNTIME:-5} FIO_SIZE=${FIO_SIZE:-1g}; fi
  size_bytes "$FIO_SIZE" >/dev/null || exit 2
  disk_check
  docker pull -q "$KVROCKS_IMAGE" >/dev/null; docker pull -q "$MEMTIER_IMAGE" >/dev/null; docker pull -q "$CLI_IMAGE" >/dev/null
  run_fio  # before any server: the disk's own 256 KiB random-read curve
  META_EXTRA=("#w7_value_size=$W7_VALUE_SIZE" "#w7_keys=$W7_KEYS" "w7_phases=$(w7_phase_list | xargs)"
              "#w7_sweep=$W7_SWEEP" "#block_cache_mb=$BLOCK_CACHE_MB" "compression=$COMPRESSION" "#client_cpu_count=$(cpu_count "$CLIENT_CPUS")"
              "#client_bound_pct=$CLIENT_BOUND_PCT" "fio_image=$FIO_IMAGE" "fio_status=$FIO_STATUS"
              "fio_qds=$FIO_QDS" "#fio_runtime_s=$FIO_RUNTIME" "fio_size=$FIO_SIZE" "w7_servers=$W7_SERVERS"
              "w7_client=--large sets memtier to $W7_THREADS threads x $W7_CLIENTS clients; THREADS and CLIENTS are ignored"
              "w7_kvrocks_extra=${KVROCKS_LARGE_CONF[*]}")
  write_meta
  local server
  for server in $W7_SERVERS; do
    run_server_large "$server" || log "$server: run failed, continuing"
    remove_data  # one server's data at a time
  done
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
  if [[ $SIZE == large* ]]; then run_large; fi
  run_full
fi
