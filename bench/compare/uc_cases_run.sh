#!/usr/bin/env bash
# Case functions of the use-case suite (spec 2026-11-12 §4): case_<CASE> SERVER REP, plus case_<CASE>_open SERVER REP
# for pass B of U1 and U3 (see uc_run.sh's header). Sourced after uc_run.sh by usecases.sh and the tests. The
# dispatcher runs them with errexit off, so every step is checked and a failure sets UC_FAIL_REASON and returns 1.
# Files per repetition, under $OUT/<CASE>/<server>/rep<N>/:
#   load/<n>.json load.seconds settle-load du-after-load   the load; seconds = memtier wall time (the DBSIZE check after
#                                                          it is not counted)
#   <phase>.{json,status,settle,cpu,io,info0,info1,stats.jsonl,client.jsonl}   one uc_cell per timed phase
#   U3: age.* age.seconds settle-age du-after-age;  U1 Kvrocks: compact.status settle-compact du-after-compact
#   pass B: reload/ reload.seconds reload.settle (U3: reload-age.*) open.rates open25|open50|open75.*
#   U5: load-<size>/ load-<size>.seconds settle-load-<size> du-after-load-<size> <size>.*
#   U8: summary.json (no server)
# du-after-* files hold the du sampler's header and one row (epoch_s,bytes,wal_archive_bytes).
# shellcheck disable=SC2034  # UC_RATE is read by uc_phase

_uc_fail() { UC_FAIL_REASON=$1; log "${CUR:-}: $1"; return 1; }

# Seconds since START_MS (date +%s%3N) with millisecond precision.
_uc_since() { local ms=$(($(date +%s%3N) - $1)); printf '%d.%03d\n' $((ms / 1000)) $((ms % 1000)); }

_uc_up() {
  local level
  level=$(uc_level "$2") || return 1
  start_server "$1" "$level" "$OUT/data/$1" || { _uc_fail "server did not start"; return 1; }
  parity_check "$1" "$level" || { _uc_fail "parity check failed; see $1/config.txt"; return 1; }
}

_uc_down() { stop_server; SERVER_CONTAINER=""; }

# shellcheck disable=SC2317  # callbacks are invoked indirectly by uc_load / uc_phase
_uc_load_cb() { memtier "$UC_LOAD_DIR/$UC_LOAD_N" "$@" || return; UC_LOAD_N=$((UC_LOAD_N + 1)); }
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

# _uc_load CASE REP NAME KEYS [U5 PHASE]: memtier load into REP/NAME/, NAME.seconds, then the DBSIZE check.
_uc_load() {
  local c=$1 rep=$2 name=$3 keys=$4 start
  UC_LOAD_DIR=$rep/$name UC_LOAD_N=1
  start=$(date +%s%3N)
  uc_load "$c" _uc_load_cb ${5:+"$5"} || { _uc_fail "load failed; see $rep/$name/"; return 1; }
  _uc_since "$start" >"$OUT/$rep/$name.seconds"
  _uc_dbsize_ok "$keys"
}

# _uc_settle_du REP NAME: settle-NAME, then du-after-NAME of the server's data dir.
_uc_settle_du() {
  settle "$OUT/$1/settle-$2" || { _uc_fail "could not read the server's cgroup to wait for it to settle"; return 1; }
  uc_du_snapshot "$OUT/data/$SERVER_NAME" >"$OUT/$1/du-after-$2"
}

# _uc_cell CASE REP PHASE: one timed phase through uc_cell (UC_CELL_WARMUP / UC_CELL_DURATION apply).
_uc_cell() {
  UC_ARGS=()
  uc_phase "$1" "$3" _uc_args_cb || { _uc_fail "no memtier arguments for $1 $3"; return 1; }
  uc_cell "$2/$3" "${UC_ARGS[@]}" || { _uc_fail "cell $3 failed: $(head -1 "$OUT/$2/$3.status" 2>/dev/null)"; return 1; }
}

# _uc_requests CASE REP PHASE NAME: a --requests phase (U3's aging) measured like a cell but not timed; NAME.seconds.
_uc_requests() {
  local start
  UC_ARGS=()
  uc_phase "$1" "$3" _uc_args_cb || { _uc_fail "no memtier arguments for $1 $3"; return 1; }
  start=$(date +%s%3N)
  MEASURE_INFO=INFO measure_memtier "$2/$4" "${UC_ARGS[@]}"
  _uc_since "$start" >"$OUT/$2/$4.seconds"
  _uc_check_alive "$2/$4"
  [[ $(head -1 "$OUT/$2/$4.status") == ok ]] || { _uc_fail "$4 failed: $(head -1 "$OUT/$2/$4.status")"; return 1; }
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
  # U8's "after a full compaction where the server offers one": TidesKV has no COMPACT
  if [[ $s == kvrocks* ]]; then _uc_compact "$rep" || return 1; fi
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
  _uc_requests U3 "$rep" age age || return 1
  _uc_settle_du "$rep" age || return 1
  UC_CELL_WARMUP=0 UC_CELL_DURATION=$UC_U3_CLOSED_S _uc_cell U3 "$rep" closed
}

# Pass B starts from the same aged state as pass A: reload, then age again.
case_U3_open() {
  local rep=U3/$1/rep$2
  _uc_up "$1" U3 || return 1
  _uc_load U3 "$rep" reload "$U3_KEYS" || return 1
  _uc_requests U3 "$rep" age reload-age || return 1
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
    _uc_up "$s" U5 || return 1
    _uc_load U5 "$rep" "load-$p" "$k" "$p" || return 1
    _uc_settle_du "$rep" "load-$p" || return 1
    _uc_cell U5 "$rep" "$p" || return 1
    _uc_down
    remove_data
  done
}

# The pass-A status of CASE SERVER REP: closed.done for a two-pass run, else done.
_uc_closed_status() {
  local d=$OUT/$1/$2/rep$3
  if [[ -e $d/closed.done ]]; then cat "$d/closed.done"; else cat "$d/done" 2>/dev/null; fi
}

# U8 runs no server: bulk-load and footprint numbers from U1's load (and Kvrocks' COMPACT) and U3's aging.
case_U8() {
  local s=$1 r=$2 c
  for c in U1 U3; do
    [[ $(_uc_closed_status "$c" "$s" "$r") == ok ]] || { _uc_fail "U8 needs $c rep$r of $s (run $c first)"; return 1; }
  done
  python3 - "$U1_KEYS" "$OUT/U1/$s/rep$r" "$OUT/U3/$s/rep$r" "$s" "$r" >"$OUT/U8/$s/rep$r/summary.json" <<'PY' ||
import json, os, sys
keys, u1, u3, server, rep = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5])

def text(path):
    with open(path) as f:
        return f.read().strip()

def du(path):
    if not os.path.exists(path):
        return None
    _, total, wal = text(path).splitlines()[1].split(",")
    return {"bytes": int(total), "wal_archive_bytes": int(wal)}

secs = float(text(os.path.join(u1, "load.seconds")))
settle = text(os.path.join(u1, "settle-load")).split()
compact = os.path.join(u1, "compact.status")
print(json.dumps({
    "server": server, "rep": rep, "load_keys": keys, "load_seconds": secs,
    "load_ops_per_s": round(keys / secs) if secs > 0 else None,
    "settle_load_s": int(settle[0]), "settle_load_capped": len(settle) > 1,
    "du_after_load": du(os.path.join(u1, "du-after-load")),
    "u3_du_after_load": du(os.path.join(u3, "du-after-load")),
    "du_after_age": du(os.path.join(u3, "du-after-age")),
    "kvrocks_after_compact": du(os.path.join(u1, "du-after-compact")),
    "compact_status": text(compact) if os.path.exists(compact) else None,
}, indent=2))
PY
    { _uc_fail "could not write U8 summary.json from U1/U3 files"; return 1; }
}
