#!/usr/bin/env bash
# Case functions of the use-case suite (spec 2026-11-12 §4): case_<CASE> SERVER REP, plus case_<CASE>_open SERVER REP
# for pass B of U1 and U3 (see uc_run.sh's header). Sourced after uc_run.sh by usecases.sh and the tests. The
# dispatcher runs them with errexit off, so every step is checked and a failure sets UC_FAIL_REASON and returns 1.
# Files per repetition, under $OUT/<CASE>/<server>/rep<N>/:
#   load/<n>.json load.seconds settle-load du-after-load   the load; seconds = uc_loader.py's wall time (the DBSIZE check after
#                                                          it is not counted)
#   <phase>.{json,status,settle,cpu,io,info0,info1,stats.jsonl,client.jsonl}   one uc_cell per timed phase
#   U3: age/ age.seconds settle-age du-after-age (the aging is a uc_loader.py run, like the loads)
#   pass B: reload/ reload.seconds reload.settle (U3: reload-age/ reload-age.seconds) open.rates open25|open50|open75.*
#   U5: load-<size>/ load-<size>.seconds settle-load-<size> du-after-load-<size> <size>.*
#   <load>/<n>.json <n>.log: uc_loader.py's result and output; <load>.pipeline: its pipeline depth (the same on every
#   server)
#   sst-after-<what>.json (Kvrocks only, next to each du-after-<what>): its live SST bytes by compression (uc_lsm.py)
#   U2: du.csv (sampler over churn and idle) churn.* du-end dbsize-end (or `unverified`) value-size idle.status
#   U6: load/ load.seconds settle-load du-after-load c1|c8|c64|c256.*
#   U7: load/ load.seconds settle-load du-after-load, then per point <sig>-<n>/ (sig term|kill, n = acknowledged writes
#       before the signal, the same on both servers: uc_cases.sh's UC_U7_ACKS), or <sig>-<n>/run<i>/ for i = 1..UC_U7_TIMES
#       when that is above 1, holding: baseline.json (probe of the preload keys in 100 ms windows, 10 s before the writer:
#       p99_ms, requests, hits), until-p99-ms (2 x its p99), ack.<thread> (the writer's acked-key logs; keys are
#       ack-r<rep>-<sig>-<n>-<run>-<thread>:<seq>, unique per point because the data dir is kept; values UC_U7_VALUE
#       bytes), acked-target (n), acked-at-signal (acked writes counted right before the signal), acked.status (ok, or why
#       n was not reached within UC_U7_ACK_TIMEOUT: the point is ERR in the report, but still signalled and verified),
#       writer.out, writer.status (exit code; probe.status likewise), signal.epoch_ms, term.seconds (TERM only: signal to
#       exit), restart.epoch_ms (t0: written by start_server right after the restart's docker run returned),
#       restart.docker_run_ms (that docker run's duration), evict-failed (only when the restart could not drop the data
#       dir from the page cache), probe.json / probe.out (pong_ms, restart_misses = requests - hits (not a failure),
#       recovered_ms = start of the first 3 consecutive passing 100 ms windows, hits; the probe container is started
#       before the signal and waits for restart.epoch_ms) and verify.json (acked, lost, lost_sample, corrupt,
#       corrupt_sample)
#   U8: memtier-load/ memtier-load.seconds settle-memtier-load du-after-memtier-load (the load speed, memtier payload);
#       Kvrocks also, on an emptied data dir: compact-load/ compact-load.seconds settle-compact-load
#       du-after-compact-load, compact.status settle-compact du-after-compact; then summary.json
#   F1: matrix.json per server (this server against Redis 7.2, run.log) and, merged over the servers done so far, F1/matrix.json
# du-after-* files hold the du sampler's header and one row (epoch_s,bytes,wal_archive_bytes,allocated_bytes,
# wal_archive_allocated_bytes: columns 2-3 apparent size, 4-5 allocated bytes).
#   U6 also: fio/fsync.json and fio/append.json (+ .log) per server, measured right before that server's sweep
# shellcheck disable=SC2034  # UC_RATE is read by uc_phase

_uc_fail() { UC_FAIL_REASON=$1; log "${CUR:-}: $1"; return 1; }

# Seconds since START_MS (date +%s%3N) with millisecond precision.
_uc_since() { local ms=$(($(date +%s%3N) - $1)); printf '%d.%03d\n' $((ms / 1000)) $((ms % 1000)); }

# _uc_note_evict DIR WHAT: start_server sets EVICT_FAILED when it could not drop the data dir from the page cache (its
# reads could then be served past SERVER_MEM); DIR/evict-failed gets a line naming the start, for the report's caveat.
_uc_note_evict() {
  if ((${EVICT_FAILED:-0})); then echo "$2: could not drop the data dir from the page cache" >>"$OUT/$1/evict-failed"; fi
  return 0
}

# _uc_up SERVER CASE [WHAT]: start SERVER at CASE's level, note a failed page-cache eviction in the rep dir (UC_REP_DIR,
# set by _uc_run) and check parity.
_uc_up() {
  local level
  level=$(uc_level "$2") || return 1
  start_server "$1" "$level" "$OUT/data/$1" || { _uc_fail "server did not start"; return 1; }
  if [[ -n ${UC_REP_DIR:-} ]]; then _uc_note_evict "$UC_REP_DIR" "${3:-start}"; fi
  parity_check "$1" "$level" || { _uc_fail "parity check failed; see $1/config.txt"; return 1; }
}

_uc_down() { stop_server; SERVER_CONTAINER=""; }

# shellcheck disable=SC2317  # callbacks are invoked indirectly by uc_load / uc_phase
_uc_load_cb() { uc_loader "$UC_LOAD_DIR/$UC_LOAD_N" "$@" || return; UC_LOAD_N=$((UC_LOAD_N + 1)); }
# shellcheck disable=SC2317
_uc_memtier_cb() { memtier "$UC_LOAD_DIR/$UC_LOAD_N" "$@" || return; UC_LOAD_N=$((UC_LOAD_N + 1)); }
# shellcheck disable=SC2317
_uc_args_cb() { UC_ARGS=("$@"); }

# DBSIZE >= 99% of KEYS (as wl_verify_cmds: memtier skips edge ids, a --command load lands keys - 1). Kvrocks' DBSIZE
# is a cache that stays 0 until a DBSIZE SCAN has run: poll it (as run.sh's load_all).
_uc_dbsize_ok() {
  local want=$(($1 * 99 / 100)) got="" i tries=1
  if [[ $SERVER_NAME == kvrocks* ]]; then cli DBSIZE SCAN >/dev/null; tries=${UC_DBSIZE_TRIES:-900}; fi
  for ((i = 0; i < tries; i++)); do
    got=$(cli DBSIZE | tr -d '\r')
    if [[ $got =~ ^[0-9]+$ ]] && ((got >= want)); then return 0; fi
    if ((i + 1 < tries)); then sleep 2; fi
  done
  _uc_fail "load check failed: DBSIZE = [$got], want >= $want"
}

# uc_loader NAME ARGS...: uc_loader.py in a PY_IMAGE container on the bench network against the running server, with
# UC_LOADER_CONNS connections at pipeline UC_LOADER_PIPELINE; result in $OUT/NAME.json, output in $OUT/NAME.log.
uc_loader() {
  local name=$1 dir c=tideskv-bench-loader-$TS-$RANDOM; shift
  dir=$(dirname "$OUT/$name")
  mkdir -p "$dir"
  _track "$c"
  docker run --rm --name "$c" --network "$NET" --cpuset-cpus "$CLIENT_CPUS" --user "$(id -u):$(id -g)" -v "$dir:/out" \
    -v "$HERE_RUN/uc_loader.py:/uc_loader.py:ro" "$PY_IMAGE" python -I /uc_loader.py --host "$SERVER_CONTAINER" \
    --port "$SERVER_PORT" --conns "$UC_LOADER_CONNS" --pipeline "$UC_LOADER_PIPELINE" \
    --out "/out/$(basename "$name").json" "$@" >"$OUT/$name.log" 2>&1
}

# _uc_load CASE REP NAME KEYS [U5 PHASE]: uc_loader.py load into REP/NAME/, NAME.seconds, then the DBSIZE check.
_uc_load() {
  local c=$1 rep=$2 name=$3 keys=$4 start
  UC_LOAD_DIR=$rep/$name UC_LOAD_N=1
  start=$(date +%s%3N)
  uc_load "$c" _uc_load_cb ${5:+"$5"} || { _uc_fail "load failed; see $rep/$name/"; return 1; }
  _uc_since "$start" >"$OUT/$rep/$name.seconds"
  echo "$UC_LOADER_PIPELINE" >"$OUT/$rep/$name.pipeline"  # the unmeasured load is pipelined (same for every server)
  _uc_dbsize_ok "$keys"
}

# _uc_memtier_load REP NAME: U8's memtier load of U1's key set into REP/NAME/, NAME.seconds, then the DBSIZE check.
_uc_memtier_load() {
  local rep=$1 name=$2 start
  UC_LOAD_DIR=$rep/$name UC_LOAD_N=1
  start=$(date +%s%3N)
  uc_memtier_load _uc_memtier_cb || { _uc_fail "memtier load failed; see $rep/$name/"; return 1; }
  _uc_since "$start" >"$OUT/$rep/$name.seconds"
  echo 32 >"$OUT/$rep/$name.pipeline"
  _uc_dbsize_ok "$U1_KEYS"
}

# _uc_age REP NAME: U3's aging (uc_age through uc_loader.py) into REP/NAME/, NAME.seconds, then a liveness check.
_uc_age() {
  local rep=$1 name=$2 start
  UC_LOAD_DIR=$rep/$name UC_LOAD_N=1
  start=$(date +%s%3N)
  uc_age _uc_load_cb || { _uc_fail "aging failed; see $rep/$name/"; return 1; }
  _uc_since "$start" >"$OUT/$rep/$name.seconds"
  echo ok >"$OUT/$rep/$name.status"
  _uc_check_alive "$rep/$name"
  [[ $(head -1 "$OUT/$rep/$name.status") == ok ]] || { _uc_fail "$name failed: $(head -1 "$OUT/$rep/$name.status")"; return 1; }
}

# _uc_settle_du REP NAME: settle-NAME, then du-after-NAME of the server's data dir and, for Kvrocks, sst-after-NAME.json
# (uc_lsm.py: how much of that is stored uncompressed). A failed uc_lsm.py leaves a note in sst-after-NAME.err only.
_uc_settle_du() {
  settle "$OUT/$1/settle-$2" || { _uc_fail "could not read the server's cgroup to wait for it to settle"; return 1; }
  uc_du_snapshot "$OUT/data/$SERVER_NAME" >"$OUT/$1/du-after-$2" || { _uc_fail "du of the data dir failed"; return 1; }
  if [[ $SERVER_NAME == kvrocks* ]]; then
    python3 "$HERE_RUN/uc_lsm.py" "$OUT/data/$SERVER_NAME/db" >"$OUT/$1/sst-after-$2.json" 2>"$OUT/$1/sst-after-$2.err" \
      || rm -f "$OUT/$1/sst-after-$2.json"
    [[ -s $OUT/$1/sst-after-$2.err ]] || rm -f "$OUT/$1/sst-after-$2.err"
  fi
  return 0
}

# _uc_cell CASE REP PHASE: one timed phase through uc_cell (UC_CELL_WARMUP / UC_CELL_DURATION apply).
_uc_cell() {
  UC_ARGS=()
  uc_phase "$1" "$3" _uc_args_cb || { _uc_fail "no memtier arguments for $1 $3"; return 1; }
  uc_cell "$2/$3" "${UC_ARGS[@]}" || { _uc_fail "cell $3 failed: $(head -1 "$OUT/$2/$3.status" 2>/dev/null)"; return 1; }
}

# _uc_open_cells CASE REP: open25/50/75 at the per-connection rates in UC_RATES (set by the dispatcher).
_uc_open_cells() {
  local rates ph i=0
  read -ra rates <<<"${UC_RATES:-}"
  ((${#rates[@]} == 3)) || { _uc_fail "no open-loop rates (UC_RATES=[${UC_RATES:-}])"; return 1; }
  for ph in open25 open50 open75; do
    UC_RATE=${rates[i]}
    i=$((i + 1))
    _uc_cell "$1" "$2" "$ph" || return 1
  done
}

# Kvrocks' COMPACT runs in the background; INFO's is_compacting is yes until it ends (UC_COMPACT_WAIT at most).
_uc_compact() {
  local rep=$1 i state="" status=ok
  [[ $(cli COMPACT | tr -d '\r') == OK ]] || { _uc_fail "COMPACT was refused"; return 1; }
  for ((i = 0; i < UC_COMPACT_WAIT; i += 5)); do
    state=$(cli INFO rocksdb | tr -d '\r' | sed -n 's/^is_compacting://p')
    [[ $state == no ]] && break
    sleep 5
  done
  [[ $state == no ]] || status="did not finish within ${UC_COMPACT_WAIT}s"
  echo "$status" >"$OUT/$rep/compact.status"
  _uc_settle_du "$rep" compact
}

case_U1() {
  local s=$1 rep=U1/$1/rep$2 ph
  _uc_up "$s" U1 || return 1
  _uc_load U1 "$rep" load "$U1_KEYS" || return 1
  _uc_settle_du "$rep" load || return 1
  for ph in closed-gauss closed-uniform; do _uc_cell U1 "$rep" "$ph" || return 1; done
}

case_U1_open() {
  local rep=U1/$1/rep$2
  _uc_up "$1" U1 || return 1
  _uc_load U1 "$rep" reload "$U1_KEYS" || return 1
  settle "$OUT/$rep/reload.settle" || { _uc_fail "could not read the server's cgroup"; return 1; }
  _uc_open_cells U1 "$rep"
}

# The closed run follows the aging directly, so it has no warm-up: the 10 s windows cover the whole run.
case_U3() {
  local rep=U3/$1/rep$2
  _uc_up "$1" U3 || return 1
  _uc_load U3 "$rep" load "$U3_KEYS" || return 1
  _uc_settle_du "$rep" load || return 1
  _uc_age "$rep" age || return 1
  _uc_settle_du "$rep" age || return 1
  UC_CELL_WARMUP=0 UC_CELL_DURATION=$UC_U3_CLOSED_S _uc_cell U3 "$rep" closed
}

# Pass B starts from the same aged state as pass A: reload, then age again.
case_U3_open() {
  local rep=U3/$1/rep$2
  _uc_up "$1" U3 || return 1
  _uc_load U3 "$rep" reload "$U3_KEYS" || return 1
  _uc_age "$rep" reload-age || return 1
  settle "$OUT/$rep/reload-age.settle" || { _uc_fail "could not read the server's cgroup"; return 1; }
  UC_CELL_WARMUP=0 UC_CELL_DURATION=$UC_U3_OPEN_S _uc_open_cells U3 "$rep"
}

case_U4() {
  local rep=U4/$1/rep$2
  _uc_up "$1" U4 || return 1
  _uc_load U4 "$rep" load "$U4_KEYS" || return 1
  _uc_settle_du "$rep" load || return 1
  _uc_cell U4 "$rep" mix
}

# Each value size on its own data: load, settle, du, run, then stop and empty the data dir before the next size.
case_U5() {
  local s=$1 rep=U5/$1/rep$2 p kv k v
  for p in $(uc_phases U5); do
    kv=$(uc_u5_keys "$p") || { _uc_fail "no key count for U5 $p"; return 1; }
    read -r k v <<<"$kv"
    _uc_up "$s" U5 "start before $p" || return 1
    _uc_load U5 "$rep" "load-$p" "$k" "$p" || return 1
    _uc_settle_du "$rep" "load-$p" || return 1
    _uc_cell U5 "$rep" "$p" || return 1
    _uc_down
    remove_data || { _uc_fail "could not empty the data dir after $p"; return 1; }
  done
}

# U2: fixed-rate SET/GET churn with expiry, then an idle period. The du sampler runs from before the churn to after the
# idle period (du.csv); du-end and dbsize-end (keys left once everything should have expired; recorded, not checked)
# are taken at the end. Kvrocks-cron differs from kvrocks only by its config line.
_uc_scan_ts() { cli INFO keyspace | tr -d '\r' | sed -n 's/^last_dbsize_scan_timestamp://p'; }

# Kvrocks 2.17: DBSIZE SCAN runs in the background and INFO keyspace's last_dbsize_scan_timestamp changes when it lands
# (checked on a live 2.17.0 container: 0 -> epoch seconds, with the key count updated at the same time). Until then DBSIZE
# is a cached 0, so wait for the timestamp to change (UC_DBSIZE_TRIES polls, UC_DBSIZE_POLL s apart); no change = "unverified".
_uc_dbsize_end() {
  local rep=$1 i before="" now="" got=""
  if [[ $SERVER_NAME == kvrocks* ]]; then
    before=$(_uc_scan_ts)
    cli DBSIZE SCAN >/dev/null
    for ((i = 0; i < ${UC_DBSIZE_TRIES:-900}; i++)); do
      now=$(_uc_scan_ts)
      if [[ $now =~ ^[0-9]+$ && $now != "$before" ]]; then got=$(cli DBSIZE | tr -d '\r'); break; fi
      sleep "${UC_DBSIZE_POLL:-2}"
    done
  else
    got=$(cli DBSIZE | tr -d '\r')
  fi
  [[ $got =~ ^[0-9]+$ ]] || got=unverified
  echo "$got" >"$OUT/$rep/dbsize-end"
}

case_U2() {
  local s=$1 rep=U2/$1/rep$2 spid rc=0
  _uc_up "$s" U2 || return 1
  uc_du_sampler "$OUT/data/$s" "$UC_U2_DU_EVERY" "$OUT/$rep/du.csv" & spid=$!
  UC_CELL_WARMUP=0 UC_CELL_DURATION=$UC_U2_WRITE_S _uc_cell U2 "$rep" churn || rc=1
  if ((rc == 0)); then
    sleep "$UC_U2_IDLE_S" & wait $!  # in the background so a TERM is handled now, as in settle
    echo ok >"$OUT/$rep/idle.status"
    _uc_check_alive "$rep/idle"
    [[ $(head -1 "$OUT/$rep/idle.status") == ok ]] || { _uc_fail "idle failed: $(head -1 "$OUT/$rep/idle.status")"; rc=1; }
  fi
  stop_sampler "$spid"
  ((rc == 0)) || return 1
  # the value size of the churn, for the report's live-bytes estimate (dbsize-end x value-size)
  printf '%s\n' "${UC_ARGS[@]}" | sed -n 's/^--data-size=//p' >"$OUT/$rep/value-size"
  uc_du_snapshot "$OUT/data/$s" >"$OUT/$rep/du-end"
  _uc_dbsize_end "$rep"
}

# U6: strict durability, fixed preload, the two fio floors per server, closed-loop connection sweep with at most 10 s of warm-up per cell.
case_U6() {
  local rep=U6/$1/rep$2 ph
  _uc_up "$1" U6 || return 1
  _uc_load U6 "$rep" load "$U6_KEYS" || return 1
  _uc_settle_du "$rep" load || return 1
  # the disk's fsync floor drifts (R24): measure it right before this server's sweep, into the rep dir
  run_fio_fsync "$OUT/$rep/fio"   # fio reference: 4 KiB random write + fsync
  run_fio_append "$OUT/$rep/fio"  # durable-append floor: 4 KiB sequential write + fdatasync
  for ph in $(uc_phases U6); do
    UC_CELL_WARMUP=$((UC_WARMUP < 10 ? UC_WARMUP : 10)) UC_CELL_DURATION=$UC_U6_S _uc_cell U6 "$rep" "$ph" || return 1
  done
}

# The pass-A status of CASE SERVER REP: closed.done for a two-pass run, else done.
_uc_closed_status() {
  local d=$OUT/$1/$2/rep$3
  if [[ -e $d/closed.done ]]; then cat "$d/closed.done"; else cat "$d/done" 2>/dev/null; fi
}

# U8: bulk load and footprint. The load speed comes from a memtier load of U1's key set (memtier-load), so it does not
# depend on uc_loader.py; its footprint is kept but not the reported one (memtier's payload compresses unrealistically).
# Kvrocks then gets an emptied data dir, U1's controlled load once more (compact-load) and a full COMPACT, for the
# steady-state footprint of the same data U1 measured after its load (TidesKV has no COMPACT). The footprints after load
# and after aging come from U1's and U3's files.
_uc_u8_kvrocks_compact() {
  local rep=$1
  _uc_down
  remove_data || { _uc_fail "could not empty the data dir before the compact load"; return 1; }
  _uc_up "$SERVER_NAME" U8 "start before the compact load" || return 1
  _uc_load U1 "$rep" compact-load "$U1_KEYS" || return 1
  _uc_settle_du "$rep" compact-load || return 1
  _uc_compact "$rep"
}

case_U8() {
  local s=$1 r=$2 c rep=U8/$1/rep$2
  for c in U1 U3; do
    [[ $(_uc_closed_status "$c" "$s" "$r") == ok ]] || { _uc_fail "U8 needs $c rep$r of $s (run $c first)"; return 1; }
  done
  _uc_up "$s" U8 || return 1
  _uc_memtier_load "$rep" memtier-load || return 1
  _uc_settle_du "$rep" memtier-load || return 1
  if [[ $s == kvrocks* ]]; then _uc_u8_kvrocks_compact "$rep" || return 1; fi
  python3 - "$U1_KEYS" "$OUT/U1/$s/rep$r" "$OUT/U3/$s/rep$r" "$s" "$r" "$OUT/$rep" >"$OUT/$rep/summary.json" <<'PY' ||
import json, os, sys
keys, u1, u3, server, rep, u8 = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]), sys.argv[6]

def text(path):
    with open(path) as f:
        return f.read().strip()

def du(path):
    if not os.path.exists(path):
        return None
    row = [int(x) for x in text(path).splitlines()[1].split(",")]
    out = {"bytes": row[1], "wal_archive_bytes": row[2]}
    if len(row) >= 5:  # allocated bytes, appended last by the du sampler
        out.update(allocated_bytes=row[3], wal_archive_allocated_bytes=row[4])
    return out

def sst(path):
    return json.load(open(path)) if os.path.exists(path) else None

def ops(d, name):
    secs = float(text(os.path.join(d, name + ".seconds")))
    return secs, (round(keys / secs) if secs > 0 else None)

secs, speed = ops(u8, "memtier-load")
_, controlled = ops(u1, "load")
settle = text(os.path.join(u1, "settle-load")).split()
compact = os.path.join(u8, "compact.status")
print(json.dumps({
    "server": server, "rep": rep, "load_keys": keys, "load_seconds": secs, "load_ops_per_s": speed,
    "controlled_load_ops_per_s": controlled,
    "settle_load_s": int(settle[0]), "settle_load_capped": len(settle) > 1,
    "du_after_load": du(os.path.join(u1, "du-after-load")),
    "memtier_du_after_load": du(os.path.join(u8, "du-after-memtier-load")),
    "u3_du_after_load": du(os.path.join(u3, "du-after-load")),
    "du_after_age": du(os.path.join(u3, "du-after-age")),
    "kvrocks_after_compact": du(os.path.join(u8, "du-after-compact")),
    "compact_status": text(compact) if os.path.exists(compact) else None,
    "sst_after_load": sst(os.path.join(u1, "sst-after-load.json")),
    "sst_after_memtier_load": sst(os.path.join(u8, "sst-after-memtier-load.json")),
    "sst_after_age": sst(os.path.join(u3, "sst-after-age.json")),
    "sst_after_compact": sst(os.path.join(u8, "sst-after-compact.json")),
}, indent=2))
PY
    { _uc_fail "could not write U8 summary.json from U1/U3 files"; return 1; }
}

# ---- U7: crash and restart, with the acknowledged-write checker (uc_ackwriter.py, run in PY_IMAGE) ----

# _uc_py DIR ARGS...: uc_ackwriter.py in a PY_IMAGE container on the bench network with $OUT/DIR as /out, as the host user.
_uc_py() {
  local dir=$1 c=tideskv-bench-py-$TS-$RANDOM; shift
  _track "$c"
  docker run --rm --name "$c" --network "$NET" --cpuset-cpus "$CLIENT_CPUS" --user "$(id -u):$(id -g)" \
    -v "$OUT/$dir:/out" -v "$HERE_RUN/uc_ackwriter.py:/uc_ackwriter.py:ro" "$PY_IMAGE" python -I /uc_ackwriter.py "$@"
}

# Waits up to SECS for container NAME to stop; returns 1 on timeout.
_uc_u7_wait_exit() {
  local name=$1 i
  for ((i = 0; i < $2 * 10; i++)); do
    [[ $(docker inspect -f '{{.State.Running}}' "$name" 2>/dev/null) == true ]] || return 0
    sleep 0.1
  done
  return 1
}

# The writer's first acknowledged write is logged: it is writing.
_uc_u7_wait_writing() {
  local i
  for ((i = 0; i < 300; i++)); do
    [[ -s $OUT/$1/ack.0 ]] && return 0
    sleep 0.1
  done
  return 1
}

# Acknowledged writes logged so far under DIR: complete lines of every ack.<thread> (a line is written after the +OK).
_uc_u7_acked() { cat "$OUT/$1"/ack.[0-9]* 2>/dev/null | wc -l; }

# _uc_u7_wait_acked DIR N SECS: polls the writer's ack logs every 50 ms until N writes are acknowledged or SECS pass.
# Writes acked-target, acked-at-signal (the count of the last poll, right before the signal) and acked.status.
_uc_u7_wait_acked() {
  local d=$1 n=$2 secs=$3 got=0 start
  echo "$n" >"$OUT/$d/acked-target"
  start=$(date +%s%3N)
  while :; do
    got=$(_uc_u7_acked "$d")
    ((got >= n)) && break
    (($(date +%s%3N) - start >= secs * 1000)) && break
    sleep 0.05
  done
  echo "$got" >"$OUT/$d/acked-at-signal"
  if ((got >= n)); then echo ok >"$OUT/$d/acked.status"; return 0; fi
  echo "load point not reached: $got of $n acknowledged writes in $secs s" >"$OUT/$d/acked.status"
  log "${CUR:-}: $d: $(<"$OUT/$d/acked.status")"
}

# _uc_u7_writer_start DIR NAME PREFIX: the ack-writer on the server, detached.
_uc_u7_writer_start() {
  _track "$2"
  docker run -d --name "$2" --network "$NET" --cpuset-cpus "$CLIENT_CPUS" --user "$(id -u):$(id -g)" \
    -v "$OUT/$1:/out" -v "$HERE_RUN/uc_ackwriter.py:/uc_ackwriter.py:ro" "$PY_IMAGE" python -I /uc_ackwriter.py write \
    --host "$SERVER_CONTAINER" --port "$SERVER_PORT" --conns "${UC_U7_CONNS:-16}" --prefix "$3" --log /out/ack --value-size "$UC_U7_VALUE" >/dev/null
}

# _uc_u7_probe_start DIR NAME UNTIL: the recovery probe, detached and started before the kill so that its own start-up
# is not in pong_ms; it waits for /out/restart.epoch_ms, then polls PING every few ms.
_uc_u7_probe_start() {
  _track "$2"
  docker run -d --name "$2" --network "$NET" --cpuset-cpus "$CLIENT_CPUS" --user "$(id -u):$(id -g)" \
    -v "$OUT/$1:/out" -v "$HERE_RUN/uc_ackwriter.py:/uc_ackwriter.py:ro" "$PY_IMAGE" python -I /uc_ackwriter.py probe \
    --host "$SERVER_CONTAINER" --port "$SERVER_PORT" --until-p99 "$3" --timeout "${UC_U7_PROBE_TIMEOUT:-300}" \
    --prefix u7: --keymax "$U7_KEYS" --t0-file /out/restart.epoch_ms --out /out/probe.json >/dev/null
}

# Waits for the detached tool container NAME, saves its output as DIR/FILE and fails with WHAT when it exited nonzero.
_uc_u7_tool_collect() {
  local d=$1 name=$2 file=$3 what=$4 rc
  _uc_u7_wait_exit "$name" "$5" || { _uc_fail "$what did not finish in $5 s"; return 1; }
  docker logs "$name" >"$OUT/$d/$file" 2>&1 || true
  rc=$(docker inspect -f '{{.State.ExitCode}}' "$name" 2>/dev/null)
  docker rm -v "$name" >/dev/null 2>&1 || true
  echo "$rc" >"$OUT/$d/${file%.out}.status"
  [[ $rc == 0 ]] || { _uc_fail "$what exited $rc: $(tail -1 "$OUT/$d/$file")"; return 1; }
}

# _uc_u7_signal DIR SIG: SIG the server container and wait (60 s at most) for it to exit; TERM's shutdown time is recorded.
_uc_u7_signal() {
  local d=$1 sig=$2 start
  date +%s%3N >"$OUT/$d/signal.epoch_ms"
  start=$(date +%s%3N)
  docker kill -s "$sig" "$SERVER_CONTAINER" >/dev/null || { _uc_fail "docker kill -s $sig failed"; return 1; }
  _uc_u7_wait_exit "$SERVER_CONTAINER" 60 || { _uc_fail "$SERVER_NAME still running 60 s after SIG$sig"; return 1; }
  if [[ $sig == TERM ]]; then _uc_since "$start" >"$OUT/$d/term.seconds"; fi
}

# One point, files in DIR (relative to $OUT): baseline probe, probe container, writer, N acked writes, signal, restart,
# probe, verify. PREFIX is unique per point (the data dir is kept, so a repeated key name could hide a lost write).
_uc_u7_point() {
  local d=$1 sig=$2 n=$3 prefix=$4 w=tideskv-bench-writer-$TS-$RANDOM p=tideskv-bench-probe-$TS-$RANDOM until
  mkdir -p "$OUT/$d"
  _uc_py "$d" probe --host "$SERVER_CONTAINER" --port "$SERVER_PORT" --baseline-secs 10 --prefix u7: \
    --keymax "$U7_KEYS" --out /out/baseline.json >/dev/null \
    || { _uc_fail "baseline probe failed (no preloaded u7: key found?)"; return 1; }
  until=$(python3 -c 'import json, sys; print(2 * json.load(open(sys.argv[1]))["p99_ms"])' "$OUT/$d/baseline.json") \
    || { _uc_fail "no p99 in $d/baseline.json"; return 1; }
  echo "$until" >"$OUT/$d/until-p99-ms"
  _uc_u7_probe_start "$d" "$p" "$until" || { _uc_fail "could not start the probe"; return 1; }
  _uc_u7_writer_start "$d" "$w" "$prefix" || { _uc_fail "could not start the writer"; return 1; }
  _uc_u7_wait_writing "$d" || { _uc_fail "the writer acked nothing in 30 s"; return 1; }
  _uc_u7_wait_acked "$d" "$n" "$UC_U7_ACK_TIMEOUT"
  _uc_u7_signal "$d" "$sig" || return 1
  _uc_u7_tool_collect "$d" "$w" writer.out "the writer" 60 || return 1
  stop_server || true  # keeps the server's log, removes the dead container
  START_NOWAIT=1 START_T0_FILE=$OUT/$d/restart.epoch_ms START_RUN_MS_FILE=$OUT/$d/restart.docker_run_ms \
    start_server "$SERVER_NAME" strict "$OUT/data/$SERVER_NAME" \
    || { _uc_fail "restart failed"; return 1; }
  _uc_note_evict "$d" "restart"
  _uc_u7_tool_collect "$d" "$p" probe.out "the recovery probe" $((${UC_U7_PROBE_TIMEOUT:-300} + 60)) || return 1
  _uc_py "$d" verify --host "$SERVER_CONTAINER" --port "$SERVER_PORT" --log /out/ack --value-size "$UC_U7_VALUE" --out /out/verify.json >/dev/null \
    || { _uc_fail "verify failed"; return 1; }
}

# U7: strict durability. One preload, then for every point (UC_U7_ACKS acknowledged writes before the signal) and
# signal (TERM, KILL), UC_U7_TIMES times: the point restarts the server on the same data dir, so the next one starts
# from the recovered server.
case_U7() {
  local s=$1 rep=U7/$1/rep$2 ph sig n i d
  _uc_up "$s" U7 || return 1
  _uc_load U7 "$rep" load "$U7_KEYS" || return 1
  _uc_settle_du "$rep" load || return 1
  for ph in $(uc_phases U7); do
    sig=${ph%-*} n=${ph#*-}
    for ((i = 1; i <= UC_U7_TIMES; i++)); do
      d=$rep/$ph
      if ((UC_U7_TIMES > 1)); then d=$d/run$i; fi
      _uc_u7_point "$d" "${sig^^}" "$n" "ack-r$2-$sig-$n-$i-" || return 1
    done
  done
}

# ---- F1: command matrix against Redis 7.2 (f1_matrix.py, run in PY_IMAGE) ----

# _uc_f1_tool REPDIR ARGS...: f1_matrix.py in a PY_IMAGE container on the bench network, files read-only, REPDIR as /out.
_uc_f1_tool() {
  local dir=$1 c=tideskv-bench-f1-$TS-$RANDOM; shift
  _track "$c"
  docker run --rm --name "$c" --network "$NET" --cpuset-cpus "$CLIENT_CPUS" --user "$(id -u):$(id -g)" -v "$OUT/$dir:/out" \
    -v "$HERE_RUN/f1_matrix.py:/f1/f1_matrix.py:ro" -v "$HERE_RUN/f1_cases.txt:/f1/f1_cases.txt:ro" \
    -v "$HERE_RUN/f1_documented.txt:/f1/f1_documented.txt:ro" "$PY_IMAGE" python -I /f1/f1_matrix.py "$@"
}

# The Redis 7.2 reference (CLI_IMAGE): no persistence, on the server's CPUs. Waits up to 30 s for PING.
_uc_f1_ref_start() {
  local i
  _track "$1"
  docker run -d --name "$1" --network "$NET" --cpuset-cpus "$SERVER_CPUS" --memory "$SERVER_MEM" "$CLI_IMAGE" \
    redis-server --save "" --appendonly no >/dev/null || return 1
  for ((i = 0; i < 30; i++)); do
    [[ $(docker exec "$1" redis-cli PING 2>/dev/null) == PONG ]] && return 0
    sleep 1
  done
  return 1
}

# F1: this server (relaxed) and a fresh Redis 7.2 get the same commands (f1_cases.txt); one run is deterministic.
# Every server's matrix lands in F1/<server>/rep1/matrix.json; F1/matrix.json merges the servers done so far.
case_F1() {
  local s=$1 rep=F1/$1/rep$2 ref=tideskv-bench-redis-$TS-$RANDOM rc=0 merged=() f
  _uc_up "$s" F1 || return 1
  if _uc_f1_ref_start "$ref"; then
    _uc_f1_tool "$rep" run --target "$s=$SERVER_CONTAINER:$SERVER_PORT" --ref "$ref:6379" --cases /f1/f1_cases.txt \
      --documented /f1/f1_documented.txt --out /out/matrix.json >"$OUT/$rep/run.log" 2>&1 || rc=1
  else
    rc=2
  fi
  docker rm -f -v "$ref" >/dev/null 2>&1 || true
  ((rc != 2)) || { _uc_fail "the Redis 7.2 reference did not answer PING"; return 1; }
  ((rc == 0)) || { _uc_fail "f1_matrix.py failed: $(tail -1 "$OUT/$rep/run.log")"; return 1; }
  for f in "$OUT"/F1/*/rep*/matrix.json; do merged+=("$f"); done
  python3 "$HERE_RUN/f1_matrix.py" merge "${merged[@]}" --out "$OUT/F1/matrix.json" \
    || { _uc_fail "could not merge the F1 matrices"; return 1; }
}
