#!/usr/bin/env bash
# Container lifecycle for bench/compare (spec 2026-10-18 §4, §8). Sourced by run.sh.
# shellcheck disable=SC2034
KVROCKS_IMAGE=apache/kvrocks:2.17.0
MEMTIER_IMAGE=redislabs/memtier_benchmark:2.1.4
CLI_IMAGE=redis:7.2.5
# W7's disk ceiling (fio 3.13). Pinned by digest; run_fio skips and says so in meta.json when it cannot be pulled.
FIO_IMAGE=xridge/fio@sha256:c58227616c9d2ef4c7d9c762a382d5921608ee3cf4b6aef7d99c4f964089d9c3
# The use-case suite's Python tools (U7's ack-writer, verify and probe) run in this image, pinned by digest (python:3.12.7-slim).
PY_IMAGE=python@sha256:60d9996b6a8a3689d36db740b49f4327be3be09a21122bd02fb8895abb38b50d
SERVER_CPUS=${SERVER_CPUS:-0-3}
CLIENT_CPUS=${CLIENT_CPUS:-8-15}
SERVER_MEM=${SERVER_MEM:-8g}
# Docker's default is soft 1024 / hard 524288: Kvrocks raises its soft limit itself, TidesKV does not and clamps its
# SST cache, so both get the same explicit limit.
SERVER_NOFILE=65536
# Parity settings both servers get; --large lowers the block cache to 1 GiB, its sweep raises workers to 16.
BLOCK_CACHE_MB=2048
SERVER_WORKERS=4
# both servers spell "off" as no; --large turns it off so stored bytes match its incompressible-data premise
COMPRESSION=lz4
# Quiet means under QUIESCE_CPU_PCT of one core and at most QUIESCE_WRITE_BYTES written to disk over QUIESCE_WINDOW
# seconds; run.sh sets QUIESCE_MAX, the longest wait.
QUIESCE_WINDOW=5
QUIESCE_CPU_PCT=5
QUIESCE_WRITE_BYTES=$((1 << 20))
KVROCKS_BLOB_CONF=("rocksdb.enable_blob_files yes" "rocksdb.min_blob_size 4096")
# --large (W7_LARGE=1) only: Kvrocks' default WAL archive (3 h TTL, 16 GiB) would hold M32's rewrites on disk and could
# fill it; W7 has no replica to keep it for
KVROCKS_LARGE_CONF=("rocksdb.wal_ttl_seconds 0" "rocksdb.wal_size_limit_mb 0")
W7_LARGE=0
# set by start_server when the data dir could not be dropped from the page cache
EVICT_FAILED=0
CONTAINERS=()
NET_CREATED=0

log() { printf '%s %s\n' "$(date -u +%H:%M:%S)" "$*" >&2; }

bench_cleanup() {
  local c j
  # background jobs (the stats sampler, a settle wait, W6's DEL memtier) would outlive an interrupted run
  for j in $(jobs -p); do pkill -P "$j" 2>/dev/null; kill "$j" 2>/dev/null; done
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
  # -exec ... + (not ';') so that a failing dd fails find, and the run can record it
  # shellcheck disable=SC2016  # $f is expanded by the container's sh
  docker run --rm --name "$c" --network none --cpuset-cpus "$CLIENT_CPUS" -v "$data:/d" --entrypoint find "$CLI_IMAGE" \
    /d -type f -exec sh -c 'for f; do dd if="$f" iflag=nocache count=0 status=none || exit 1; done' sh '{}' + \
    >/dev/null 2>&1 || { log "could not evict $data from the page cache"; return 1; }
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

# Writes the server's config for a level into $OUT/<server>/ and prints its path. SERVER_WORKERS and BLOCK_CACHE_MB
# replace the parity values; kvrocks-blob is the Kvrocks config plus BlobDB (the Percona post's W7 setup).
write_server_conf() {
  local server=$1 level=$2 conf mode
  mkdir -p "$OUT/$server"
  if [[ $server == tideskv ]]; then
    mode=$([[ $level == strict ]] && echo full || echo "$RELAXED")
    conf="$OUT/$server/tideskv-$level.conf"
    local cache=${BLOCK_CACHE_MB}mb
    if ((BLOCK_CACHE_MB % 1024 == 0)); then cache=$((BLOCK_CACHE_MB / 1024))gb; fi
    sed -e "s/SYNC_MODE/$mode/" -e "s/^workers .*/workers $SERVER_WORKERS/" \
        -e "s/^block-cache-size .*/block-cache-size $cache/" -e "s/^compression .*/compression $COMPRESSION/" \
        "$HERE/tideskv.conf" >"$conf" || return
    # TIDESKV_EXTRA_CONF (env): extra config lines separated by ';', appended last so they win (experiments)
    if [[ -n ${TIDESKV_EXTRA_CONF:-} ]]; then tr ';' '\n' <<<"$TIDESKV_EXTRA_CONF" >>"$conf"; fi
  else
    mode=$([[ $level == strict ]] && echo yes || echo no)
    conf="$OUT/$server/kvrocks-$level.conf"
    sed -e "s/SYNC_YESNO/$mode/" -e "s/^workers .*/workers $SERVER_WORKERS/" \
        -e "s/^rocksdb.block_cache_size .*/rocksdb.block_cache_size $BLOCK_CACHE_MB/" \
        -e "s/^rocksdb.compression .*/rocksdb.compression $COMPRESSION/" "$HERE/kvrocks.conf" >"$conf" || return
    if [[ $server == kvrocks-blob ]]; then printf '%s\n' "${KVROCKS_BLOB_CONF[@]}" >>"$conf"; fi
    # UC_EXTRA_CONF (array, use-case suite): more "key value" lines, skipped when already in the conf
    local line
    for line in ${UC_EXTRA_CONF[@]+"${UC_EXTRA_CONF[@]}"}; do
      grep -qxF -- "$line" "$conf" || echo "$line" >>"$conf"
    done
    if ((W7_LARGE)); then printf '%s\n' "${KVROCKS_LARGE_CONF[@]}" >>"$conf"; fi
  fi
  echo "$conf"
}

start_server() {
  local server=$1 level=$2 data=$3 conf
  mkdir -p "$data" "$OUT/$server"
  EVICT_FAILED=0
  evict_cache "$data" || EVICT_FAILED=1
  chmod 0777 "$data"  # Kvrocks runs as uid 999 in its image (plan ruling 4)
  SERVER_NAME=$server
  SERVER_CONTAINER=tideskv-bench-$server-$TS
  _track "$SERVER_CONTAINER"
  local common=(--detach --name "$SERVER_CONTAINER" --network "$NET" --cpuset-cpus "$SERVER_CPUS"
                --memory "$SERVER_MEM" --memory-swap "$SERVER_MEM" --ulimit "nofile=$SERVER_NOFILE:$SERVER_NOFILE")
  conf=$(write_server_conf "$server" "$level") || return 1
  local t_before t_after
  t_before=$(date +%s%3N)
  if [[ $server == tideskv ]]; then
    SERVER_PORT=6379
    docker run "${common[@]}" -v "$data:/data" -v "$conf:/etc/tideskv.conf:ro" "$TIDESKV_IMAGE" /etc/tideskv.conf >/dev/null
  else
    SERVER_PORT=6666
    docker run "${common[@]}" -v "$data:/var/lib/kvrocks/data" -v "$conf:/etc/kvrocks.conf:ro" \
      --entrypoint kvrocks "$KVROCKS_IMAGE" -c /etc/kvrocks.conf >/dev/null
  fi
  t_after=$(date +%s%3N)
  # U7's restart: START_T0_FILE gets the epoch ms right after docker run returned (the process is exec'd by then; container
  # create and network attach are not in it), written atomically for a waiting probe; START_RUN_MS_FILE gets docker run's
  # own duration in ms
  if [[ -n ${START_T0_FILE:-} ]]; then echo "$t_after" >"$START_T0_FILE.tmp" && mv "$START_T0_FILE.tmp" "$START_T0_FILE"; fi
  if [[ -n ${START_RUN_MS_FILE:-} ]]; then echo $((t_after - t_before)) >"$START_RUN_MS_FILE"; fi
  # START_NOWAIT=1 (U7's restart): return at once, so the caller's probe measures the time to the first PONG itself
  if ((${START_NOWAIT:-0})); then log "$server ($level) started, not waiting for it"; return 0; fi
  local i
  for ((i = 0; i < 60; i++)); do
    if [[ $(cli PING 2>/dev/null) == PONG ]]; then
      SERVER_CGROUP=/sys/fs/cgroup$(sed -n 's/^0:://p' "/proc/$(docker inspect -f '{{.State.Pid}}' "$SERVER_CONTAINER")/cgroup")
      # cgroup v2 only: an empty path would read host-wide CPU and writes as the server's
      if [[ $SERVER_CGROUP == /sys/fs/cgroup || $SERVER_CGROUP == /sys/fs/cgroup/ || ! -f $SERVER_CGROUP/cpu.stat ]]; then
        log "cannot find $server's cgroup v2 directory ($SERVER_CGROUP)"; return 1
      fi
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
    echo "workers $SERVER_WORKERS"; echo "block-cache-size $((BLOCK_CACHE_MB << 20))"; echo "memtable-size 268435456"
    echo "compression $COMPRESSION"; echo "sync-mode $([[ $level == strict ]] && echo full || echo "$RELAXED")"
  else
    # a line that a later source repeats (kvrocks-blob's, via UC_EXTRA_CONF) is checked once
    {
      echo "workers $SERVER_WORKERS"; echo "rocksdb.block_cache_size $BLOCK_CACHE_MB"; echo "rocksdb.write_buffer_size 256"
      echo "rocksdb.compression $COMPRESSION"; echo "rocksdb.write_options.sync $([[ $level == strict ]] && echo yes || echo no)"
      if [[ $server == kvrocks-blob ]]; then printf '%s\n' "${KVROCKS_BLOB_CONF[@]}"; fi
      if ((W7_LARGE)); then printf '%s\n' "${KVROCKS_LARGE_CONF[@]}"; fi
      if [[ -n ${UC_EXTRA_CONF[*]+x} ]]; then printf '%s\n' "${UC_EXTRA_CONF[@]}"; fi
    } | awk '!seen[$0]++'
  fi
}

parity_check() {
  local server=$1 level=$2 key want got bad=0
  echo "# $level${3:+ $3}" >>"$OUT/$server/config.txt"
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
  # MEMTIER_CNAME names the container so a sampler can find it (W7's client CPU); it must carry $TS for bench_cleanup
  local c=${MEMTIER_CNAME:-tideskv-bench-memtier-$TS-$RANDOM}
  _track "$c"
  docker run --rm --name "$c" --network "$NET" --cpuset-cpus "$CLIENT_CPUS" -v "$dir:/out" "$MEMTIER_IMAGE" \
    -s "$SERVER_CONTAINER" -p "$SERVER_PORT" --protocol=redis --hide-histogram \
    --print-percentiles=50,99,99.9 --json-out-file="/out/$(basename "$name").json" "$@" \
    >"$OUT/$name.log" 2>&1
}

# ---- W7 (--large) measurements ----

# Number of CPUs in a cpuset list such as 0-3,8.
cpu_count() {
  local part n=0 parts
  IFS=, read -ra parts <<<"$1"
  for part in "${parts[@]}"; do
    if [[ $part == *-* ]]; then n=$((n + ${part#*-} - ${part%-*} + 1)); else n=$((n + 1)); fi
  done
  echo "$n"
}

# The server cgroup's io.stat as it stands; report.py takes the busiest device's delta between two snapshots.
cg_io_snapshot() { cat "$SERVER_CGROUP/io.stat" 2>/dev/null || true; }

# Append-only (run_w7_phase writes "M start|end <epoch ms>" markers into the same file).
# Once a second: "T <epoch ms>" then /proc/diskstats for the block devices (no loop/ram/zram). report.py computes
# aqu-sz (Δweighted ms / Δwall ms) and r_await (Δread ms / Δreads) for the device io.stat names busiest.
# The counters are the device's, host-wide: they include any other I/O on the host.
diskstats_sampler() {
  while kill -0 $$ 2>/dev/null; do
    printf 'T %s\n' "$(date +%s%3N)"
    awk '$3 !~ /^(loop|ram|zram)/' /proc/diskstats
    sleep 1
  done
}

# docker stats of one container every 2 s, one JSON line per sample (peak memory, the client's CPU).
stats_sampler() {
  while kill -0 $$ 2>/dev/null; do docker stats --no-stream --format '{{json .}}' "$1" 2>/dev/null; sleep 2; done
}

# Bytes in a size such as 8g, 512m, 4096 (fio's and docker's k/m/g suffixes, powers of 1024).
size_bytes() {
  local n=${1%[kKmMgGtT]} u=${1: -1}
  [[ $n =~ ^[0-9]+$ ]] || { echo "size_bytes: bad size $1" >&2; return 2; }
  case $u in
    k|K) echo $((n << 10)) ;; m|M) echo $((n << 20)) ;; g|G) echo $((n << 30)) ;; t|T) echo $((n << 40)) ;;
    *) echo "$n" ;;
  esac
}

# fio 256 KiB random reads at each queue depth in FIO_QDS, FIO_RUNTIME s each, O_DIRECT on a FIO_SIZE file under
# $OUT/fio (the bench filesystem). Writes $OUT/fio/qd<N>.json and sets FIO_STATUS (ok, skipped: ..., failed ...).
run_fio() {
  local dir=$OUT/fio qd c rc=0
  if ! docker image inspect "$FIO_IMAGE" >/dev/null 2>&1 && ! docker pull -q "$FIO_IMAGE" >/dev/null 2>&1; then
    log "fio: cannot pull $FIO_IMAGE; skipping the disk ceiling"
    FIO_STATUS="skipped: cannot pull $FIO_IMAGE"; return 0
  fi
  mkdir -p "$dir"; chmod 0777 "$dir"
  for qd in $FIO_QDS; do
    c=tideskv-bench-fio-$TS-qd$qd
    _track "$c"
    log "fio QD $qd"
    docker run --rm --name "$c" --network none --cpuset-cpus "$CLIENT_CPUS" -v "$dir:/f" --entrypoint fio "$FIO_IMAGE" \
      --name="qd$qd" --filename=/f/fio.dat --size="$FIO_SIZE" --rw=randread --bs=256k --direct=1 --ioengine=libaio \
      --iodepth="$qd" --numjobs=1 --runtime="$FIO_RUNTIME" --time_based --output-format=json --output="/f/qd$qd.json" \
      >"$dir/qd$qd.log" 2>&1 || { rc=$?; log "fio QD $qd failed ($rc); see fio/qd$qd.log"; break; }
  done
  # the file is root's; delete it from a container
  docker run --rm --name "tideskv-bench-clean-fio-$TS" --network none -v "$dir:/f" --entrypoint rm "$CLI_IMAGE" \
    -f /f/fio.dat >/dev/null 2>&1 || log "could not delete $dir/fio.dat"
  if ((rc)); then FIO_STATUS="failed at QD $qd (exit $rc)"; else FIO_STATUS=ok; fi
}

# _fio_flush_job DIR NAME ARGS...: one fio job of 4 KiB buffered writes at queue depth 1 with a flush after each (ARGS
# give --rw and the flush), FIO_FSYNC_RUNTIME s on a FIO_FSYNC_SIZE file under DIR. Writes DIR/NAME.json (+ .log) and
# deletes its file; returns fio's exit status, or 3 when the fio image cannot be pulled.
_fio_flush_job() {
  local dir=$1 name=$2 c=tideskv-bench-fio-$TS-$2-$RANDOM rc=0; shift 2
  if ! docker image inspect "$FIO_IMAGE" >/dev/null 2>&1 && ! docker pull -q "$FIO_IMAGE" >/dev/null 2>&1; then
    log "fio: cannot pull $FIO_IMAGE; skipping the $name job"
    return 3
  fi
  mkdir -p "$dir"; chmod 0777 "$dir"
  _track "$c"
  log "fio $name"
  docker run --rm --name "$c" --network none --cpuset-cpus "$CLIENT_CPUS" -v "$dir:/f" --entrypoint fio "$FIO_IMAGE" \
    --name="$name" --filename="/f/fio-$name.dat" --size="${FIO_FSYNC_SIZE:-1g}" --bs=4k --direct=0 --ioengine=sync \
    --iodepth=1 --numjobs=1 --runtime="${FIO_FSYNC_RUNTIME:-30}" --time_based --output-format=json \
    --output="/f/$name.json" "$@" >"$dir/$name.log" 2>&1 || { rc=$?; log "fio $name failed ($rc); see fio/$name.log"; }
  docker run --rm --name "${c/bench-fio/bench-clean-fio}" --network none -v "$dir:/f" --entrypoint rm "$CLI_IMAGE" \
    -f "/f/fio-$name.dat" >/dev/null 2>&1 || log "could not delete $dir/fio-$name.dat"
  return "$rc"
}

_fio_status() { case $1 in 0) echo ok ;; 3) echo "skipped: cannot pull $FIO_IMAGE" ;; *) echo "failed (exit $1)" ;; esac; }

# The fio reference of U6: 4 KiB random writes with an fsync after each (queue depth 1), under DIR (default $OUT/fio).
# Writes DIR/fsync.json and sets FIO_FSYNC_STATUS (ok, skipped: ..., failed ...).
run_fio_fsync() {
  local rc=0
  _fio_flush_job "${1:-$OUT/fio}" fsync --rw=randwrite --fsync=1 || rc=$?
  FIO_FSYNC_STATUS=$(_fio_status "$rc")
}

# The durable-append floor of U6: 4 KiB sequential writes with an fdatasync after each (queue depth 1), the pattern of a
# write-ahead log. Writes DIR/append.json and sets FIO_APPEND_STATUS.
run_fio_append() {
  local rc=0
  _fio_flush_job "${1:-$OUT/fio}" append --rw=write --fdatasync=1 || rc=$?
  FIO_APPEND_STATUS=$(_fio_status "$rc")
}

# ---- shared by run.sh and the use-case runner ----

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

_du() { du -sb "$1" | cut -f1; }

# The servers write the data dirs as other uids, so the host user cannot delete them: empty them from a container.
# Returns 1 when that failed (run.sh ignores it, as before; the use-case runner records it).
remove_data() {
  [[ -d $OUT/data ]] || return 0
  docker run --rm --name "tideskv-bench-clean-$TS" -v "$OUT/data:/d" --entrypoint find "$CLI_IMAGE" \
    /d -mindepth 1 -delete >/dev/null 2>&1 || { log "could not empty $OUT/data"; return 1; }
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

# The timed-run measurement shared by run.sh's run_workload and the use-case runner: starts the docker stats sampler,
# samples the server cgroup around one memtier run (NAME is relative to $OUT, the rest are memtier arguments) and writes
# NAME.{json,stats.jsonl,cpu,io,status}, plus .info0/.info1 (INFO persistence) when $CUR is tideskv. MEASURE_INFO (the
# use-case suite sets "INFO") replaces that: those INFO arguments, on every server.
measure_memtier() {
  local base=$1 stats_pid cpu0 cpu1 info=(INFO persistence) want_info=0; shift
  if [[ ${CUR:-} == tideskv ]]; then want_info=1; fi
  if [[ -n ${MEASURE_INFO:-} ]]; then read -ra info <<<"$MEASURE_INFO"; want_info=1; fi
  # docker stats gives peak memory; CPU is the cgroup's usage_usec delta over exactly the timed run
  ( while kill -0 $$ 2>/dev/null; do docker stats --no-stream --format '{{json .}}' "$SERVER_CONTAINER"; sleep 2; done ) \
    >"$OUT/$base.stats.jsonl" 2>/dev/null & stats_pid=$!
  if ((want_info)); then cli "${info[@]}" 2>/dev/null | tr -d '\r' >"$OUT/$base.info0" || true; fi
  cpu0=$(cg_sample) || cpu0=""
  if memtier "$base" "$@"; then
    echo ok >"$OUT/$base.status"
  else
    echo "memtier exited $?; see $base.log" >"$OUT/$base.status"
  fi
  cpu1=$(cg_sample) || cpu1=""
  if [[ -n $cpu0 && -n $cpu1 ]]; then
    echo $(( ${cpu1%% *} - ${cpu0%% *} )) >"$OUT/$base.cpu"
    echo $(( ${cpu1##* } - ${cpu0##* } )) >"$OUT/$base.io"
  fi
  if ((want_info)); then cli "${info[@]}" 2>/dev/null | tr -d '\r' >"$OUT/$base.info1" || true; fi
  stop_sampler "$stats_pid"
}

# Fields every run's meta.json carries, one key=value per line ("#key=value" is an integer); uses DIRTY, TS, QUIESCE_MAX.
meta_common_fields() {
  local kv_digest sha
  kv_digest=$(docker image inspect -f '{{index .RepoDigests 0}}' "$KVROCKS_IMAGE" 2>/dev/null || echo "$KVROCKS_IMAGE")
  sha=$(git -C "$ROOT" rev-parse --short HEAD)
  if ((${DIRTY:-0})); then sha+=-dirty; fi
  printf '%s\n' "tideskv_sha=$sha" "kvrocks_image=$kv_digest" "tideskv_image=${TIDESKV_IMAGE:-}" "memtier_image=$MEMTIER_IMAGE" \
    "server_cpus=$SERVER_CPUS" "server_cores=$(server_cores)" "server_mem=$SERVER_MEM" "#server_nofile=$SERVER_NOFILE" \
    "client_cpus=$CLIENT_CPUS" "memtier_run_flags=${MEMTIER_RUN_FLAGS[*]}" \
    "#quiesce_window_s=$QUIESCE_WINDOW" "#quiesce_cpu_pct=$QUIESCE_CPU_PCT" \
    "#quiesce_write_bytes=$QUIESCE_WRITE_BYTES" "#quiesce_max_s=${QUIESCE_MAX:-600}" \
    "host=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs), kernel $(uname -r)" "started_utc=${TS:-}"
}

# write_meta_kv FILE key=value...: writes a flat JSON object; "#key=value" is an integer, other values are strings.
write_meta_kv() {
  python3 - "$@" <<'PY'
import json, sys
meta = {}
for arg in sys.argv[2:]:
    key, value = arg.split("=", 1)
    meta[key.lstrip("#")] = int(value) if key.startswith("#") else value
with open(sys.argv[1], "w") as f:
    json.dump(meta, f, indent=2)
PY
}
