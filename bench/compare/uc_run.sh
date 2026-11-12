#!/usr/bin/env bash
# Use-case suite runner helpers (spec 2026-11-12 §5). Sourced by usecases.sh and the tests; defines functions only.
# Layout: $OUT/<CASE>/<server>/rep<N>/ holds one repetition's files; its `done` file is written last and holds `ok`
# or the failure reason. case_<CASE> SERVER REP functions (Tasks 4-6, 9) run one repetition; set UC_FAIL_REASON
# before returning non-zero to put a reason in `done`; they live in uc_cases_run.sh. The dispatcher runs a case with
# errexit disabled (it is called in an `||` list), so every step of a case_<CASE> must be checked explicitly:
# `step || { UC_FAIL_REASON="..."; return 1; }`.
# uc_cell returns 1 whenever the cell's .status is not `ok` (memtier failure, or the server stopped during the cell).
# Two-pass cases (U1 and U3 outside quick mode: a case_<CASE>_open function exists) run pass A (case_<CASE>: load and
# closed phases) for every server of a repetition, writing closed.done, then pass B (case_<CASE>_open: reload and the
# open-loop phases at UC_RATES, from uc_open_rates of that repetition) for every server, writing done. Pass B owns the
# rep dir's open* and reload* files. A server whose closed.done is missing redoes pass A, and then every server of that
# repetition redoes pass B, so the open rates always come from both servers' closed results of the same repetition.
# shellcheck disable=SC2034  # globals are read by the case functions and lib.sh
HERE_RUN=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=${ROOT:-$(cd "$HERE_RUN/../.." && pwd)}
# shellcheck source-path=SCRIPTDIR source=lib.sh
source "$HERE_RUN/lib.sh"
# shellcheck source-path=SCRIPTDIR source=uc_cases.sh
source "$HERE_RUN/uc_cases.sh"

# Unlike lib.sh's: a non-empty data/ is what a resumed run has; usecases.sh empties it with remove_data.
resolve_out() { realpath -m -- "$1"; }

# "rep server" lines in run order: all servers of repetition 1, then of repetition 2, ...
uc_rep_order() {
  local r s
  for ((r = 1; r <= UC_REPS; r++)); do
    for s in $(uc_servers "$1"); do echo "$r $s"; done
  done
}

uc_is_done() { [[ -e $OUT/$1/$2/rep$3/done ]]; }

# Rewrites NAME.status when the server container stopped during NAME (OOM or crash).
_uc_check_alive() {
  if [[ $(docker inspect -f '{{.State.Running}}' "$SERVER_CONTAINER" 2>/dev/null) != true ]]; then
    echo "server stopped during the run (OOM-killed: $(docker inspect -f '{{.State.OOMKilled}}' "$SERVER_CONTAINER" 2>/dev/null))" \
      >"$OUT/$1.status"
  fi
}

# Settles the server, runs an optional warm-up (UC_CELL_WARMUP seconds, default UC_WARMUP, 0 = none) and then one timed
# run of UC_CELL_DURATION seconds (default UC_DURATION). NAME is relative to $OUT; the rest are one phase's memtier
# arguments from uc_phase. Writes NAME.{json,stats.jsonl,cpu,io,status,settle}, NAME.info0/.info1 (full INFO before
# and after, on every server: background debt, cache counters) and NAME.client.jsonl (docker stats of the timed run's
# memtier container, so the report can flag a client-bound cell, spec §5.7).
uc_cell() {
  local name=$1 warm=${UC_CELL_WARMUP-${UC_WARMUP:-0}} dur=${UC_CELL_DURATION-${UC_DURATION:-0}} mc cpid; shift
  mkdir -p "$(dirname "$OUT/$name")"
  settle "$OUT/$name.settle" || { echo "could not read the server's cgroup to wait for it to settle" >"$OUT/$name.status"; return 1; }
  if ((warm > 0)); then memtier "$name.warmup" "$@" --test-time="$warm" || true; fi
  mc=tideskv-bench-memtier-$TS-${name//\//-}
  stats_sampler "$mc" >"$OUT/$name.client.jsonl" & cpid=$!
  MEASURE_INFO=INFO MEMTIER_CNAME=$mc measure_memtier "$name" "$@" --test-time="$dur"
  stop_sampler "$cpid"
  _uc_check_alive "$name"
  [[ $(head -1 "$OUT/$name.status") == ok ]]
}

# epoch_s,bytes,wal_archive_bytes every INTERVAL seconds until the shell exits; Kvrocks' archive/ WAL dir feeds the
# third column (no such dir under TidesKV: 0).
uc_du_sampler() {
  local dir=$1 every=$2 csv=$3
  echo "epoch_s,bytes,wal_archive_bytes" >"$csv"
  while kill -0 $$ 2>/dev/null; do
    uc_du_row "$dir" >>"$csv"
    sleep "$every"
  done
}

# One "epoch_s,bytes,wal_archive_bytes" row for DIR now.
uc_du_row() {
  local dir=$1 total wal=0 a
  total=$(du -sb "$dir" 2>/dev/null | cut -f1) || total=0
  while read -r a; do wal=$((wal + $(du -sb "$a" 2>/dev/null | cut -f1))); done \
    < <(find "$dir" -maxdepth 4 -type d -name archive 2>/dev/null)
  echo "$(date +%s),${total:-0},$wal"
}

# The sampler's header and one row: the du-after-* files of the cases.
uc_du_snapshot() { echo "epoch_s,bytes,wal_archive_bytes"; uc_du_row "$1"; }

# _uc_mark_done REPDIR TEXT [FILE]: FILE (default done) in $OUT/REPDIR holds TEXT.
_uc_mark_done() { echo "$2" >"$OUT/$1/${3:-done}"; }

# Skips every pending repetition of CASE when the disk cannot hold it (done = the reason); returns 1 then.
_uc_disk_ok() {
  local c=$1 need free r s
  need=$(uc_disk_need "$c")
  free=${DISK_FREE_OVERRIDE:-$(df --output=avail -B1 "$OUT" | tail -1)}
  ((free >= need)) && return 0
  log "case $c: not enough disk (need $need bytes, $free free under $OUT)"
  while read -r r s; do
    uc_is_done "$c" "$s" "$r" && continue
    mkdir -p "$OUT/$c/$s/rep$r"
    _uc_mark_done "$c/$s/rep$r" "skipped: needs $(((need + UC_GIB - 1) / UC_GIB)) GiB free"
  done < <(uc_rep_order "$c")
  return 1
}

# _uc_run CASE REP SERVER FUNC MARK: FUNC SERVER REP on an empty data dir, then MARK (ok or the reason) in the rep dir.
# The server is stopped afterwards only if FUNC left one running (start_server sets SERVER_CONTAINER).
_uc_run() {
  local c=$1 r=$2 s=$3 fn=$4 mark=$5 rc=0
  CUR=$s UC_FAIL_REASON="" SERVER_CONTAINER=""
  remove_data
  mapfile -t UC_EXTRA_CONF < <(uc_conf_lines "$s")
  log "$c $s rep$r$([[ $fn == *_open ]] && echo " open phases")"
  "$fn" "$s" "$r" || rc=$?
  if [[ -n $SERVER_CONTAINER ]]; then stop_server || true; fi
  if ((rc == 0)); then _uc_mark_done "$c/$s/rep$r" ok "$mark"
  else _uc_mark_done "$c/$s/rep$r" "${UC_FAIL_REASON:-$fn exited $rc}" "$mark"; fi
}

# A fresh rep dir for CASE SERVER REP.
_uc_fresh() {
  rm -rf "${OUT:?}/${1:?}/${2:?}/rep${3:?}"
  mkdir -p "$OUT/$1/$2/rep$3"
}

# One repetition: wipe its leftovers and the data dir, run the case, then write `done` (ok or the reason).
_uc_rep() {
  _uc_fresh "$1" "$3" "$2"
  _uc_run "$1" "$2" "$3" "case_$1" "done"
}

_uc_two_pass() { [[ ${UC_MODE:-full} != quick ]] && declare -F "case_$1_open" >/dev/null; }

# Pass B of one server: the open phases, once every server's pass A of the repetition is ok (see the header).
_uc_pass_b() {
  local c=$1 r=$2 s=$3 o dir
  dir=${OUT:?}/${1:?}/${3:?}/rep${2:?}
  rm -rf "$dir"/open* "$dir"/reload*
  if [[ $(<"$dir/closed.done") != ok ]]; then _uc_mark_done "$c/$s/rep$r" "$(<"$dir/closed.done")"; return; fi
  for o in $(uc_servers "$c"); do
    if [[ $(cat "$OUT/$c/$o/rep$r/closed.done" 2>/dev/null) != ok ]]; then
      _uc_mark_done "$c/$s/rep$r" "open phases not run: $o rep$r closed phases did not finish ok"; return
    fi
  done
  UC_RATES=$(uc_open_rates "$c" "$r") || { _uc_mark_done "$c/$s/rep$r" "open phases not run: no closed-loop rate"; return; }
  echo "$UC_RATES" >"$dir/open.rates"
  _uc_run "$c" "$r" "$s" "case_${c}_open" "done"
}

_uc_dispatch_two_pass() {
  local c=$1 r s servers redo
  read -ra servers <<<"$(uc_servers "$c")"
  for ((r = 1; r <= UC_REPS; r++)); do
    redo=0
    for s in "${servers[@]}"; do
      [[ -e $OUT/$c/$s/rep$r/closed.done ]] && continue
      redo=1
      _uc_fresh "$c" "$s" "$r"
      _uc_run "$c" "$r" "$s" "case_$c" closed.done
    done
    if ((redo)); then for s in "${servers[@]}"; do rm -f "${OUT:?}/${c:?}/${s:?}/rep${r:?}/done"; done; fi
    for s in "${servers[@]}"; do
      if uc_is_done "$c" "$s" "$r"; then log "$c $s rep$r already done"; continue; fi
      _uc_pass_b "$c" "$r" "$s"
    done
  done
}

uc_dispatch() {
  local c=$1 r s
  if ! declare -F "case_$c" >/dev/null; then log "case $c not implemented, skipping"; return 0; fi
  _uc_disk_ok "$c" || return 0
  if _uc_two_pass "$c"; then
    _uc_dispatch_two_pass "$c"
  else
    while read -r r s; do
      if uc_is_done "$c" "$s" "$r"; then log "$c $s rep$r already done"; continue; fi
      _uc_rep "$c" "$r" "$s"
    done < <(uc_rep_order "$c")
  fi
  remove_data
}

# Per-connection --rate-limiting values of CASE's open25/50/75 phases in repetition REP: 25/50/75% of the lower of the
# servers' closed-loop ops/s in that repetition, over UC_THREADS x UC_CLIENTS connections, at least 1. The closed phase
# read is the one with the open phases' key pattern (U1 closed-gauss, U3 closed). Returns 1 on a missing or 0 result.
uc_open_rates() {
  local c=$1 r=$2 base s files=()
  case $c in
    U1) base=closed-gauss ;;
    U3) base=closed ;;
    *) echo "uc_open_rates: $c has no open-loop phases" >&2; return 2 ;;
  esac
  for s in $(uc_servers "$c"); do files+=("$OUT/$c/$s/rep$r/$base.json"); done
  python3 - "$((UC_THREADS * UC_CLIENTS))" "${files[@]}" <<'PY'
import json, sys
conns, low = int(sys.argv[1]), None
for path in sys.argv[2:]:
    try:
        with open(path) as f:
            ops = float(json.load(f)["ALL STATS"]["Totals"]["Ops/sec"])
    except (OSError, ValueError, KeyError, TypeError) as e:
        sys.exit(f"uc_open_rates: no ops/s in {path}: {e}")
    low = ops if low is None else min(low, ops)
if not low or low <= 0:
    sys.exit("uc_open_rates: a closed-loop result has 0 ops/s")
print(" ".join(str(max(1, int(low * pct / 100 / conns))) for pct in (25, 50, 75)))
PY
}

# meta.json: the shared fields plus mode, reps, cases, cache_accounting and fio_fsync_status.
uc_write_meta() {
  local common
  mapfile -t common < <(meta_common_fields)
  write_meta_kv "$OUT/meta.json" "${common[@]}" \
    "size=$UC_MODE" "mode=$UC_MODE" "#reps=$UC_REPS" "cases=$UC_CASES_RUN" "cache_accounting=$UC_CACHE_ACCOUNTING" \
    "fio_fsync_status=${FIO_FSYNC_STATUS:-not run}" "#warmup_s=$UC_WARMUP" "#duration_s=$UC_DURATION" \
    "#threads=$UC_THREADS" "#clients=$UC_CLIENTS"
}
