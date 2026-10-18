#!/usr/bin/env bash
# Container lifecycle for bench/compare (spec 2026-10-18 §4, §8). Sourced by run.sh.
# shellcheck disable=SC2034
KVROCKS_IMAGE=apache/kvrocks:2.17.0
MEMTIER_IMAGE=redislabs/memtier_benchmark:2.1.4
CLI_IMAGE=redis:7.2.5
SERVER_CPUS=${SERVER_CPUS:-0-3}
CLIENT_CPUS=${CLIENT_CPUS:-8-11}
SERVER_MEM=${SERVER_MEM:-8g}
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

bench_init() {
  TS=$(date -u +%Y%m%dT%H%M%SZ)
  OUT=${1:-$ROOT/build/bench/$TS}
  NET=tideskv-bench-$TS
  mkdir -p "$OUT"
  trap bench_cleanup EXIT
  trap 'exit 130' INT TERM
  docker network create "$NET" >/dev/null
  NET_CREATED=1
}

build_tideskv_image() {
  local sha
  sha=$(git -C "$ROOT" rev-parse --short HEAD)
  if [[ -n $(git -C "$ROOT" status --porcelain -- src cmake CMakeLists.txt) ]]; then
    if ((!DIRTY)); then log "uncommitted changes under src/ or cmake/; commit them or pass --dirty"; return 2; fi
    TIDESKV_IMAGE=tideskv-bench:$sha-dirty
    (cd "$ROOT" && git ls-files -z | tar --null -T - -c) \
      | docker build -q -t "$TIDESKV_IMAGE" -f bench/compare/tideskv.Dockerfile - >/dev/null
    return
  fi
  TIDESKV_IMAGE=tideskv-bench:$sha
  if ! docker image inspect "$TIDESKV_IMAGE" >/dev/null 2>&1; then
    log "building $TIDESKV_IMAGE"
    git -C "$ROOT" archive HEAD | docker build -q -t "$TIDESKV_IMAGE" -f bench/compare/tideskv.Dockerfile - >/dev/null
  fi
}

_track() { CONTAINERS+=("$1"); }

start_server() {
  local server=$1 level=$2 data=$3 conf mode
  mkdir -p "$data" "$OUT/$server"
  chmod 0777 "$data"  # Kvrocks runs as uid 999 in its image (plan ruling 4)
  SERVER_NAME=$server
  SERVER_CONTAINER=tideskv-bench-$server-$TS
  _track "$SERVER_CONTAINER"
  local common=(--detach --name "$SERVER_CONTAINER" --network "$NET" --cpuset-cpus "$SERVER_CPUS"
                --memory "$SERVER_MEM" --memory-swap "$SERVER_MEM")
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
    if [[ $(cli PING 2>/dev/null) == PONG ]]; then log "$server ($level) is up"; return 0; fi
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
}

cli() {
  docker run --rm --network "$NET" "$CLI_IMAGE" redis-cli -h "$SERVER_CONTAINER" -p "$SERVER_PORT" --raw "$@"
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
