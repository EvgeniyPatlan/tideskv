#!/usr/bin/env bash
# Profile TidesKV under bench/compare workloads with perf (frame-pointer build, call graphs).
# Usage: bench/compare/profile.sh [--quick|--medium] [--strict] [--seconds N] --out DIR W...   (W is W1..W6)
#   Loads the data set once, then profiles each workload at the relaxed level. --strict then restarts the server in
#   strict mode and profiles the same workloads again. perf samples for --seconds (default 30), starting 15 s into
#   the timed run. Size defaults to medium. env: WARMUP/DURATION (default 40/60 here), plus run.sh's variables.
#   Output: DIR/perf/<level>-<W>.{data,flat.txt,tree.txt,threads.txt}, DIR/info-<level>-<W>-{before,after}.txt and
#   the usual memtier results under DIR/tideskv/<level>/.
# shellcheck disable=SC2034  # SIZE, DIRTY and friends are read by run.sh and lib.sh
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

usage() { sed -n 2,8p "${BASH_SOURCE[0]}"; }
PSIZE=--medium STRICT=0 SECONDS_PERF=30 OUT_ARG="" PWLS=()
while (($#)); do
  case $1 in
    --quick|--medium) PSIZE=$1 ;;
    --strict) STRICT=1 ;;
    --seconds) SECONDS_PERF=${2:?--seconds needs a value}; shift ;;
    --out) OUT_ARG=${2:?--out needs a value}; shift ;;
    -h|--help) usage; exit 0 ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) PWLS+=("$1") ;;
  esac
  shift
done
[[ $SECONDS_PERF =~ ^[1-9][0-9]*$ ]] || { echo "--seconds must be a positive integer" >&2; exit 2; }
[[ -n $OUT_ARG ]] || { echo "--out DIR is required" >&2; usage >&2; exit 2; }
((${#PWLS[@]})) || { echo "name at least one workload (W1..W6)" >&2; exit 2; }
for w in "${PWLS[@]}"; do
  [[ $w =~ ^W[1-6]$ ]] || { echo "workload must be one of W1..W6, got: $w" >&2; exit 2; }
done

export WARMUP=${WARMUP:-40} DURATION=${DURATION:-60}
PERF_DELAY=15
if ((PERF_DELAY + SECONDS_PERF > DURATION)); then
  echo "warning: perf runs $PERF_DELAY + $SECONDS_PERF s but the timed run lasts only $DURATION s" >&2
fi

# run.sh parses its own options when sourced and defines the helpers without running anything
set -- "$PSIZE" --out "$OUT_ARG"
# shellcheck source=run.sh
source "$HERE/run.sh"

# Both images are tagged by content, so a rerun on an unchanged tree reuses them. The server image is built from the
# committed tree (git archive), with the profiling Dockerfile injected at the archive root.
build_prof_images() {
  local tag
  if [[ -n $(git -C "$ROOT" status --porcelain -- "${IMAGE_PATHS[@]}" bench/compare/prof-server.Dockerfile \
               bench/compare/prof-perf.Dockerfile) ]]; then
    log "uncommitted changes under the image paths; commit them first (profiling builds from the committed tree)"
    return 2
  fi
  tag=$(cd "$ROOT" && git rev-parse HEAD:src HEAD:cmake HEAD:CMakeLists.txt HEAD:CMakePresets.json HEAD:third_party \
          HEAD:bench/compare/prof-server.Dockerfile HEAD:bench/compare/prof-perf.Dockerfile | sha256sum | cut -c1-12)
  # ALLOCATOR (env, default mimalloc) selects TIDESKV_ALLOCATOR for an allocator A/B; it is part of the image tag
  local alloc=${ALLOCATOR:-mimalloc}
  [[ $alloc == mimalloc ]] || tag=$tag-$alloc
  PROF_IMAGE=tideskv-prof:$tag PERF_IMAGE=tideskv-perf:$tag
  if ! docker image inspect "$PROF_IMAGE" >/dev/null 2>&1; then
    log "building $PROF_IMAGE"
    (cd "$ROOT" && git archive HEAD --add-file=bench/compare/prof-server.Dockerfile -- \
        src cmake CMakeLists.txt CMakePresets.json third_party) \
      | docker build -q -t "$PROF_IMAGE" --build-arg "TIDESKV_ALLOCATOR=$alloc" -f prof-server.Dockerfile - >/dev/null
  fi
  if ! docker image inspect "$PERF_IMAGE" >/dev/null 2>&1; then
    log "building $PERF_IMAGE"
    docker build -q -t "$PERF_IMAGE" --build-arg "BASE=$PROF_IMAGE" - <"$ROOT/bench/compare/prof-perf.Dockerfile" >/dev/null
  fi
}

# Samples the server for SECONDS_PERF s from a sidecar in its pid namespace, then writes three reports. $1 = label.
# The sidecar holds profiling capabilities only; IPC_LOCK is for perf's mlock'd ring buffers (-m 256).
perf_one() {
  local d="$OUT/perf"
  mkdir -p "$d"
  docker run --rm --name "tideskv-bench-perf-$TS-$RANDOM" --pid="container:$SERVER_CONTAINER" --cpuset-cpus 4-7 \
    --cap-add PERFMON --cap-add SYS_PTRACE --cap-add IPC_LOCK -v "$d:/out" "$PERF_IMAGE" sh -c "
      perf record -F 499 --call-graph fp -m 256 -p 1 -o /out/$1.data -- sleep $SECONDS_PERF >/dev/null 2>&1
      perf report -i /out/$1.data --no-children --stdio --percent-limit 0.7 -g none --sort dso,sym 2>/dev/null > /out/$1.flat.txt
      perf report -i /out/$1.data --children --stdio --percent-limit 2 -g caller,0.5,callee --sort sym 2>/dev/null > /out/$1.tree.txt
      perf report -i /out/$1.data --no-children --stdio --percent-limit 0.5 -g none --sort comm 2>/dev/null > /out/$1.threads.txt
    "
}

# Starts perf PERF_DELAY s into the timed run. run_workload creates <w>.stats.jsonl as the timed run starts.
perf_when_timed() {
  local marker=$1 label=$2 i
  for ((i = 0; i < WARMUP + 900; i++)); do
    [[ -e $marker ]] && break
    sleep 1
  done
  sleep "$PERF_DELAY"
  perf_one "$label"
}

prof_wl() {  # $1 level, $2 workload
  cli INFO all | tr -d '\r' >"$OUT/info-$1-$2-before.txt" || true
  perf_when_timed "$OUT/$CUR/$1/$2.stats.jsonl" "$1-$2" & local p=$!
  log "tideskv $1 $2"
  run_workload "$1" "$2" || true
  wait "$p" || log "perf failed for $1 $2"
  cli INFO all | tr -d '\r' >"$OUT/info-$1-$2-after.txt" || true
}

run_profile() {
  local w
  wl_sizes "$SIZE"
  disk_check
  docker pull -q "$MEMTIER_IMAGE" >/dev/null; docker pull -q "$CLI_IMAGE" >/dev/null
  build_prof_images || exit 2
  TIDESKV_IMAGE=$PROF_IMAGE
  CUR=tideskv
  start_server tideskv relaxed "$OUT/data/tideskv"
  load_all
  settle "$OUT/tideskv/settle-load"
  for w in "${PWLS[@]}"; do prof_wl relaxed "$w"; done
  if ((STRICT)); then
    stop_server
    start_server tideskv strict "$OUT/data/tideskv"
    settle "$OUT/tideskv/settle-strict"
    for w in "${PWLS[@]}"; do prof_wl strict "$w"; done
  fi
  stop_server
  remove_data || true
  log "profiles: $OUT/perf"
}

bench_init "$OUT_ARG"
run_profile
