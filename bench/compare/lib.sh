#!/usr/bin/env bash
# Container lifecycle for bench/compare (spec 2026-10-18 §4, §8). Sourced by run.sh.
# shellcheck disable=SC2034
KVROCKS_IMAGE=apache/kvrocks:2.17.0
MEMTIER_IMAGE=redislabs/memtier_benchmark:2.1.4
CLI_IMAGE=redis:7.2.5
SERVER_CPUS=${SERVER_CPUS:-0-3}
CLIENT_CPUS=${CLIENT_CPUS:-8-11}
SERVER_MEM=${SERVER_MEM:-8g}
# Docker's default is soft 1024 / hard 524288: Kvrocks raises its soft limit itself, TidesKV does not and clamps its
# SST cache, so both get the same explicit limit.
SERVER_NOFILE=65536
# Quiet means under QUIESCE_CPU_PCT of one core and at most QUIESCE_WRITE_BYTES written to disk over QUIESCE_WINDOW
# seconds; run.sh sets QUIESCE_MAX, the longest wait.
QUIESCE_WINDOW=5
QUIESCE_CPU_PCT=5
QUIESCE_WRITE_BYTES=$((1 << 20))
CONTAINERS=()
NET_CREATED=0

log() { printf '%s %s\n' "$(date -u +%H:%M:%S)" "$*" >&2; }

bench_cleanup() {
  local c
  for c in "${CONTAINERS[@]}"; do docker rm -f "$c" >/dev/null 2>&1 || true; done
  # containers started from background subshells (W6's DEL memtier) carry this run's $TS in their name
  for c in $(docker ps -aq --filter "name=tideskv-bench-.*-$TS"); do docker rm -f "$c" >/dev/null 2>&1 || true; done
  if ((NET_CREATED)); then docker network rm "$NET" >/dev/null 2>&1 || true; fi
}

# Prints the run directory as an absolute path (docker -v needs one). Refuses a non-empty data/ in it: the servers
# would start on stale data, and remove_data empties data/ as root.
resolve_out() {
  local out
  out=$(realpath -m -- "$1") || return 1
  if [[ -d $out/data && -n $(find "$out/data" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null) ]]; then
    log "$out/data is not empty; remove it or pick another --out"
    return 1
  fi
  echo "$out"
}

bench_init() {
  TS=$(date -u +%Y%m%dT%H%M%SZ)
  OUT=$(resolve_out "${1:-$ROOT/build/bench/$TS}") || exit 2
  NET=tideskv-bench-$TS
  mkdir -p "$OUT"
  trap bench_cleanup EXIT
  trap 'exit 130' INT TERM
  docker network create "$NET" >/dev/null
  NET_CREATED=1
}

# Everything the image is built from; the tag, the dirty check and the build context share this list.
IMAGE_PATHS=(src cmake CMakeLists.txt CMakePresets.json third_party bench/compare/tideskv.Dockerfile)

build_tideskv_image() {
  local tag
  if [[ -n $(git -C "$ROOT" status --porcelain -- "${IMAGE_PATHS[@]}") ]]; then
    if ((!DIRTY)); then log "uncommitted changes under ${IMAGE_PATHS[*]}; commit them or pass --dirty"; return 2; fi
    TIDESKV_IMAGE=tideskv-bench:$(git -C "$ROOT" rev-parse --short HEAD)-dirty
    log "building $TIDESKV_IMAGE from the working tree"
    (cd "$ROOT" && git ls-files -z --cached --others --exclude-standard -- "${IMAGE_PATHS[@]}" | tar --null -T - -c) \
      | docker build -q -t "$TIDESKV_IMAGE" -f bench/compare/tideskv.Dockerfile - >/dev/null
    return
  fi
  # tagged by content, so a commit that touches only bench scripts reuses the image
  tag=$(cd "$ROOT" && git rev-parse HEAD:src HEAD:cmake HEAD:CMakeLists.txt HEAD:CMakePresets.json \
          HEAD:third_party HEAD:bench/compare/tideskv.Dockerfile | sha256sum | cut -c1-12)
  TIDESKV_IMAGE=tideskv-bench:$tag
  if ! docker image inspect "$TIDESKV_IMAGE" >/dev/null 2>&1; then
    log "building $TIDESKV_IMAGE"
    git -C "$ROOT" archive HEAD -- "${IMAGE_PATHS[@]}" \
      | docker build -q -t "$TIDESKV_IMAGE" -f bench/compare/tideskv.Dockerfile - >/dev/null
  fi
}

_track() { CONTAINERS+=("$1"); }

# Drops the data dir's files from the host page cache. Pages a removed container read stay cached, charged to its dead
# memory cgroup, so without this the next container could read past SERVER_MEM. sync first: dirty pages are not dropped.
evict_cache() {
  local data=$1
  sync
  [[ -d $data ]] || return 0
  # shellcheck disable=SC2016  # $f is expanded by the inner sh
  if find "$data" -type f -exec sh -c 'for f; do dd if="$f" iflag=nocache count=0 status=none || exit 1; done' sh {} + \
       2>/dev/null; then
    return 0
  fi
  # some files are not readable by this user (the servers write as root or uid 999): evict from a container
  local c=tideskv-bench-evict-$TS
  _track "$c"
  docker run --rm --name "$c" --network none --cpuset-cpus "$CLIENT_CPUS" -v "$data:/d" --entrypoint find "$CLI_IMAGE" \
    /d -type f -exec dd 'if={}' iflag=nocache count=0 status=none ';' >/dev/null 2>&1 \
    || log "could not evict $data from the page cache"
}

# Cumulative "<cpu usage_usec> <bytes written>" of the server container, read from its cgroup on the host.
# io.stat lists each block device of a stack (dm and nvme); the busiest one's count is used.
cg_sample() {
  local cpu w
  cpu=$(awk '$1 == "usage_usec" {print $2}' "$SERVER_CGROUP/cpu.stat" 2>/dev/null) || return 1
  w=$(awk '{for (i = 2; i <= NF; i++) if (sub(/^wbytes=/, "", $i) && $i + 0 > m) m = $i + 0} END {printf "%.0f\n", m}' \
        "$SERVER_CGROUP/io.stat" 2>/dev/null) || return 1
  [[ $cpu =~ ^[0-9]+$ && $w =~ ^[0-9]+$ ]] || return 1
  echo "$cpu $w"
}

_q_now() { echo "$SECONDS"; }
_q_sleep() { sleep 1; }

# Waits until the server is quiet (see QUIESCE_*), sampling once a second. Prints the seconds waited, with " capped"
# when QUIESCE_MAX ran out first. Engine-agnostic: only the container's CPU time and disk writes are read.
quiesce() {
  local start now c w n cpus=() writes=()
  start=$(_q_now)
  while :; do
    read -r c w < <(cg_sample) || return 1
    [[ -n $c ]] || return 1
    cpus+=("$c"); writes+=("$w"); n=${#cpus[@]}
    now=$(_q_now)
    if ((n > QUIESCE_WINDOW)) &&
       (( (c - cpus[n - 1 - QUIESCE_WINDOW]) * 100 < QUIESCE_CPU_PCT * QUIESCE_WINDOW * 1000000 )) &&
       (( w - writes[n - 1 - QUIESCE_WINDOW] <= QUIESCE_WRITE_BYTES )); then
      echo $((now - start)); return 0
    fi
    if ((now - start >= QUIESCE_MAX)); then echo "$((now - start)) capped"; return 0; fi
    _q_sleep
  done
}

start_server() {
  local server=$1 level=$2 data=$3 conf mode
  mkdir -p "$data" "$OUT/$server"
  evict_cache "$data"
  chmod 0777 "$data"  # Kvrocks runs as uid 999 in its image (plan ruling 4)
  SERVER_NAME=$server
  SERVER_CONTAINER=tideskv-bench-$server-$TS
  _track "$SERVER_CONTAINER"
  local common=(--detach --name "$SERVER_CONTAINER" --network "$NET" --cpuset-cpus "$SERVER_CPUS"
                --memory "$SERVER_MEM" --memory-swap "$SERVER_MEM" --ulimit "nofile=$SERVER_NOFILE:$SERVER_NOFILE")
  if [[ $server == tideskv ]]; then
    mode=$([[ $level == strict ]] && echo full || echo "$RELAXED")
    conf="$OUT/$server/tideskv-$level.conf"
    sed "s/SYNC_MODE/$mode/" "$HERE/tideskv.conf" >"$conf"
    SERVER_PORT=6379
    docker run "${common[@]}" -v "$data:/data" -v "$conf:/etc/tideskv.conf:ro" "$TIDESKV_IMAGE" /etc/tideskv.conf >/dev/null
  else
    mode=$([[ $level == strict ]] && echo yes || echo no)
    conf="$OUT/$server/kvrocks-$level.conf"
    sed "s/SYNC_YESNO/$mode/" "$HERE/kvrocks.conf" >"$conf"
    SERVER_PORT=6666
    docker run "${common[@]}" -v "$data:/var/lib/kvrocks/data" -v "$conf:/etc/kvrocks.conf:ro" \
      --entrypoint kvrocks "$KVROCKS_IMAGE" -c /etc/kvrocks.conf >/dev/null
  fi
  local i
  for ((i = 0; i < 60; i++)); do
    if [[ $(cli PING 2>/dev/null) == PONG ]]; then
      SERVER_CGROUP=/sys/fs/cgroup$(sed -n 's/^0:://p' "/proc/$(docker inspect -f '{{.State.Pid}}' "$SERVER_CONTAINER")/cgroup")
      log "$server ($level) is up"; return 0
    fi
    if [[ $(docker inspect -f '{{.State.Running}}' "$SERVER_CONTAINER" 2>/dev/null) != true ]]; then break; fi
    sleep 1
  done
  log "$server did not answer PING; last log lines:"
  docker logs --tail 30 "$SERVER_CONTAINER" >&2 || true
  return 1
}

stop_server() {
  docker logs "$SERVER_CONTAINER" >>"$OUT/$SERVER_NAME/server.log" 2>&1 || true
  docker stop -t 60 "$SERVER_CONTAINER" >/dev/null 2>&1 || true
  docker rm "$SERVER_CONTAINER" >/dev/null 2>&1 || true
  # flush this server's dirty pages now, not into the next phase or the other server's run
  sync
}

cli() {
  docker run --rm --name "tideskv-bench-cli-$TS-$RANDOM" --network "$NET" --cpuset-cpus "$CLIENT_CPUS" "$CLI_IMAGE" redis-cli -h "$SERVER_CONTAINER" -p "$SERVER_PORT" --raw "$@"
}

# The keys and values each server must report back (spec §4 table).
_parity_expect() {
  local server=$1 level=$2
  if [[ $server == tideskv ]]; then
    echo "workers 4"; echo "block-cache-size 2147483648"; echo "memtable-size 67108864"
    echo "compression lz4"; echo "sync-mode $([[ $level == strict ]] && echo full || echo "$RELAXED")"
  else
    echo "workers 4"; echo "rocksdb.block_cache_size 2048"; echo "rocksdb.write_buffer_size 64"
    echo "rocksdb.compression lz4"; echo "rocksdb.write_options.sync $([[ $level == strict ]] && echo yes || echo no)"
  fi
}

parity_check() {
  local server=$1 level=$2 key want got bad=0
  echo "# $level" >>"$OUT/$server/config.txt"
  while read -r key want; do
    got=$(cli CONFIG GET "$key" | sed -n 2p)
    echo "$key $got" >>"$OUT/$server/config.txt"
    if [[ $got != "$want" ]]; then log "parity: $server $key is [$got], expected [$want]"; bad=1; fi
  done < <(_parity_expect "$server" "$level")
  return $bad
}

memtier() {
  local name=$1; shift
  local dir; dir=$(dirname "$OUT/$name")
  mkdir -p "$dir"
  chmod 0777 "$dir"  # memtier writes its JSON as root inside the container
  local c=tideskv-bench-memtier-$TS-$RANDOM
  _track "$c"
  docker run --rm --name "$c" --network "$NET" --cpuset-cpus "$CLIENT_CPUS" -v "$dir:/out" "$MEMTIER_IMAGE" \
    -s "$SERVER_CONTAINER" -p "$SERVER_PORT" --protocol=redis --hide-histogram \
    --print-percentiles=50,99,99.9 --json-out-file="/out/$(basename "$name").json" "$@" \
    >"$OUT/$name.log" 2>&1
}
