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

bench_init "$OUT_ARG"
build_tideskv_image
if ((SMOKE)); then run_smoke; exit; fi
run_full
