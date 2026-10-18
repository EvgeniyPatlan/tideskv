#!/usr/bin/env bash
# TidesKV vs Kvrocks comparison benchmark (notes spec 2026-10-18).
# Usage: bench/compare/run.sh [--quick] [--smoke] [--relaxed-mode none|interval] [--dirty] [--out DIR]
#   env: SERVER_CPUS SERVER_MEM CLIENT_CPUS WARMUP DURATION THREADS CLIENTS
# shellcheck source-path=SCRIPTDIR disable=SC2034  # ROOT, SIZE and DIRTY are read by lib.sh and the phases Task 4 adds
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
# shellcheck source=workloads.sh
source "$HERE/workloads.sh"
# shellcheck source=lib.sh
source "$HERE/lib.sh"

SIZE=full RELAXED=none DIRTY=0 SMOKE=0 OUT_ARG=""
while (($#)); do
  case $1 in
    --quick) SIZE=quick ;;
    --smoke) SMOKE=1 ;;
    --relaxed-mode) RELAXED=$2; shift ;;
    --dirty) DIRTY=1 ;;
    --out) OUT_ARG=$2; shift ;;
    -h|--help) sed -n 2,4p "$0"; exit 0 ;;
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

if [[ $SIZE == quick ]]; then WARMUP=${WARMUP:-10} DURATION=${DURATION:-30}; else WARMUP=${WARMUP:-60} DURATION=${DURATION:-180}; fi
THREADS=${THREADS:-4}
CLIENTS=${CLIENTS:-50}

# Refuses when free disk is under 2.5x the logical dataset for both servers (spec §8). DISK_FREE_OVERRIDE is a test hook.
disk_check() {
  local need free
  need=$(( $(wl_logical_bytes) * 5 ))  # 2.5x per server, two servers
  free=${DISK_FREE_OVERRIDE:-$(df --output=avail -B1 "$OUT" | tail -1)}
  if (( free < need )); then log "not enough disk: need $need bytes, $free free under $OUT"; return 1; fi
}

write_meta() {
  local kv_digest
  kv_digest=$(docker image inspect -f '{{index .RepoDigests 0}}' "$KVROCKS_IMAGE" 2>/dev/null || echo "$KVROCKS_IMAGE")
  python3 - "$OUT/meta.json" <<EOF
import json, sys
json.dump({"tideskv_sha": "$(git -C "$ROOT" rev-parse --short HEAD)$( ((DIRTY)) && echo -dirty)",
           "kvrocks_image": "$kv_digest", "memtier_image": "$MEMTIER_IMAGE", "size": "$SIZE",
           "relaxed_mode": "$RELAXED", "warmup_s": $WARMUP, "duration_s": $DURATION,
           "server_cpus": "$SERVER_CPUS", "server_mem": "$SERVER_MEM",
           "host": "$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs), kernel $(uname -r)",
           "started_utc": "$TS"}, open(sys.argv[1], "w"), indent=2)
EOF
}

# wl_load supplies its own --threads/--clients (ruling T2-R1)
# shellcheck disable=SC2317  # callbacks are invoked indirectly by wl_load / wl_run
_load_cb() { memtier "$CUR/load/$LOAD_NAME-$LOAD_N" "$@"; LOAD_N=$((LOAD_N + 1)); }

load_all() {
  local ds start line want got
  start=$(date +%s)
  for ds in $DATASETS; do
    LOAD_NAME=$ds LOAD_N=1
    log "loading $ds"
    wl_load "$ds" _load_cb || { log "load of $ds failed; see $CUR/load/"; return 1; }
  done
  while read -r want line; do
    # shellcheck disable=SC2086
    got=$(cli $line)
    if ! [[ $got =~ ^[0-9]+$ ]] || (( got < want )); then log "load check failed: $line = [$got], want >= $want"; return 1; fi
  done < <(wl_verify_cmds)
  LOAD_SECONDS=$(( $(date +%s) - start ))
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
  wl_run "$w" _run_cb
  local common=(--threads="$THREADS" --clients="$CLIENTS" --pipeline=1)
  memtier "$base.warmup" "${common[@]}" --test-time="$WARMUP" "${RUN_ARGS[@]}" || true
  if [[ $w == W6 ]]; then
    wl_run_w6_del _del_cb
    memtier "$CUR/$level/W6-del" --test-time="$DURATION" "${DEL_ARGS[@]}" & del_pid=$!
  fi
  ( while :; do docker stats --no-stream --format '{{json .}}' "$SERVER_CONTAINER"; sleep 2; done ) \
    >"$OUT/$base.stats.jsonl" 2>/dev/null & stats_pid=$!
  if memtier "$base" "${common[@]}" --test-time="$DURATION" "${RUN_ARGS[@]}"; then
    echo ok >"$OUT/$base.status"
  else
    echo "memtier exited $?; see $base.log" >"$OUT/$base.status"
  fi
  kill "$stats_pid" 2>/dev/null || true; wait "$stats_pid" 2>/dev/null || true
  if [[ -n $del_pid ]]; then wait "$del_pid" || echo "W6 DEL memtier failed; see $CUR/$level/W6-del.log" >"$OUT/$base.status"; fi
  if [[ $(docker inspect -f '{{.State.Running}}' "$SERVER_CONTAINER" 2>/dev/null) != true ]]; then
    echo "server stopped during the run (OOM-killed: $(docker inspect -f '{{.State.OOMKilled}}' "$SERVER_CONTAINER"))" >"$OUT/$base.status"
    return 1
  fi
}

_du() { du -sb "$1" | cut -f1; }

run_server() {
  local server=$1 level w after
  CUR=$server
  local data="$OUT/data/$server"
  start_server "$server" relaxed "$data" || { echo "server did not start" >"$OUT/$server/relaxed.failed"; return 1; }
  parity_check "$server" relaxed || return 1
  load_all || return 1
  after=$(_du "$data")
  # relaxed and strict deliberately share one data dir (spec §5 step 4)
  for level in relaxed strict; do
    if [[ $level == strict ]]; then
      stop_server
      start_server "$server" strict "$data" || { echo "server did not start" >"$OUT/$server/strict.failed"; return 1; }
      parity_check "$server" strict || return 1
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
  write_meta
  docker pull -q "$KVROCKS_IMAGE" >/dev/null; docker pull -q "$MEMTIER_IMAGE" >/dev/null; docker pull -q "$CLI_IMAGE" >/dev/null
  local server
  for server in tideskv kvrocks; do run_server "$server" || log "$server: run failed, continuing"; done
  remove_data
  local rc=0
  python3 "$HERE/report.py" "$OUT" || rc=$?
  log "report: $OUT/report.md"
  exit $rc
}

bench_init "$OUT_ARG"
build_tideskv_image
if ((SMOKE)); then run_smoke; exit; fi
run_full
