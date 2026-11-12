#!/usr/bin/env bash
# Use-case suite runner.
# Usage: bench/compare/usecases.sh [--quick] [--dirty] [--out DIR] [CASE ...]   (CASE: U1..U8, F1; default all)
#   --out of an existing run resumes it: repetitions with a `done` file are skipped, the rest are redone
#   env: SERVER_CPUS 0-3, SERVER_MEM 8g, CLIENT_CPUS 8-15, QUIESCE_MAX 600 (quick 60), as for run.sh
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source-path=SCRIPTDIR source=uc_run.sh
source "$HERE/uc_run.sh"
# shellcheck source-path=SCRIPTDIR source=uc_cases_run.sh
source "$HERE/uc_cases_run.sh"

UC_MODE=full DIRTY=0 OUT_ARG="" REQUESTED=()
while (($#)); do
  case $1 in
    --quick) UC_MODE=quick ;;
    --dirty) DIRTY=1 ;;
    --out) OUT_ARG=${2:?--out needs a value}; shift ;;
    -h|--help) sed -n 2,5p "$0"; exit 0 ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) REQUESTED+=("$1") ;;
  esac
  shift
done
for c in "${REQUESTED[@]}"; do
  [[ " $(uc_cases) " == *" $c "* ]] || { echo "unknown case: $c (known: $(uc_cases))" >&2; exit 2; }
done
UC_CASES_RUN=${REQUESTED[*]:-$(uc_cases)}
uc_sizes "$UC_MODE"
if [[ $UC_MODE == quick ]]; then QUIESCE_MAX=${QUIESCE_MAX:-60}; else QUIESCE_MAX=${QUIESCE_MAX:-600}; fi

bench_init "$OUT_ARG"
build_tideskv_image
docker pull -q "$KVROCKS_IMAGE" >/dev/null; docker pull -q "$MEMTIER_IMAGE" >/dev/null; docker pull -q "$CLI_IMAGE" >/dev/null
if [[ " $UC_CASES_RUN " == *" U6 "* ]]; then
  if [[ $UC_MODE == quick ]]; then FIO_FSYNC_RUNTIME=5 FIO_FSYNC_SIZE=256m; else FIO_FSYNC_RUNTIME=30 FIO_FSYNC_SIZE=1g; fi
  run_fio_fsync  # before any server: the disk's own durable 4 KiB write latency
fi
remove_data  # a resumed run starts every repetition on an empty data/
uc_write_meta
for c in $UC_CASES_RUN; do uc_dispatch "$c"; done
log "use-case run finished: $OUT"
