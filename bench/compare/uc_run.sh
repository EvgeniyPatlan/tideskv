#!/usr/bin/env bash
# Use-case suite runner helpers (spec 2026-11-12 §5). Sourced by usecases.sh and the tests; defines functions only.
# Layout: $OUT/<CASE>/<server>/rep<N>/ holds one repetition's files; its `done` file is written last and holds `ok`
# or the failure reason. case_<CASE> SERVER REP functions (Tasks 4-6, 9) run one repetition; set UC_FAIL_REASON
# before returning non-zero to put a reason in `done`.
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

# Settles the server, runs an optional warm-up (UC_CELL_WARMUP seconds, default UC_WARMUP, 0 = none) and then one timed
# run of UC_CELL_DURATION seconds (default UC_DURATION). NAME is relative to $OUT; the rest are one phase's memtier
# arguments from uc_phase. Writes NAME.{json,stats.jsonl,cpu,io,status,settle}.
uc_cell() {
  local name=$1 warm=${UC_CELL_WARMUP-${UC_WARMUP:-0}} dur=${UC_CELL_DURATION-${UC_DURATION:-0}}; shift
  mkdir -p "$(dirname "$OUT/$name")"
  settle "$OUT/$name.settle" || { echo "could not read the server's cgroup to wait for it to settle" >"$OUT/$name.status"; return 1; }
  if ((warm > 0)); then memtier "$name.warmup" "$@" --test-time="$warm" || true; fi
  measure_memtier "$name" "$@" --test-time="$dur"
}

# epoch_s,bytes,wal_archive_bytes every INTERVAL seconds until the shell exits; Kvrocks' archive/ WAL dir feeds the
# third column (no such dir under TidesKV: 0).
uc_du_sampler() {
  local dir=$1 every=$2 csv=$3 total wal a
  echo "epoch_s,bytes,wal_archive_bytes" >"$csv"
  while kill -0 $$ 2>/dev/null; do
    total=$(du -sb "$dir" 2>/dev/null | cut -f1) || total=0
    wal=0
    while read -r a; do wal=$((wal + $(du -sb "$a" 2>/dev/null | cut -f1))); done \
      < <(find "$dir" -maxdepth 4 -type d -name archive 2>/dev/null)
    echo "$(date +%s),${total:-0},$wal" >>"$csv"
    sleep "$every"
  done
}

_uc_mark_done() { echo "$2" >"$OUT/$1/done"; }

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

# One repetition: wipe its leftovers and the data dir, run the case, then write `done` (ok or the reason).
_uc_rep() {
  local c=$1 r=$2 s=$3 rc=0
  CUR=$s UC_FAIL_REASON=""
  rm -rf "${OUT:?}/$c/$s/rep$r"
  mkdir -p "$OUT/$c/$s/rep$r"
  remove_data
  mapfile -t UC_EXTRA_CONF < <(uc_conf_lines "$s")
  log "$c $s rep$r"
  "case_$c" "$s" "$r" || rc=$?
  stop_server || true
  if ((rc == 0)); then _uc_mark_done "$c/$s/rep$r" ok
  else _uc_mark_done "$c/$s/rep$r" "${UC_FAIL_REASON:-case_$c exited $rc}"; fi
}

uc_dispatch() {
  local c=$1 r s
  if ! declare -F "case_$c" >/dev/null; then log "case $c not implemented, skipping"; return 0; fi
  _uc_disk_ok "$c" || return 0
  while read -r r s; do
    if uc_is_done "$c" "$s" "$r"; then log "$c $s rep$r already done"; continue; fi
    _uc_rep "$c" "$r" "$s"
  done < <(uc_rep_order "$c")
  remove_data
}

# meta.json: run.sh's fields plus mode, reps, cases, cache_accounting and fio_fsync_status.
uc_write_meta() {
  local kv_digest sha
  kv_digest=$(docker image inspect -f '{{index .RepoDigests 0}}' "$KVROCKS_IMAGE" 2>/dev/null || echo "$KVROCKS_IMAGE")
  sha=$(git -C "$ROOT" rev-parse --short HEAD)
  if ((${DIRTY:-0})); then sha+=-dirty; fi
  python3 - "$OUT/meta.json" \
    "tideskv_sha=$sha" "kvrocks_image=$kv_digest" "tideskv_image=${TIDESKV_IMAGE:-}" "memtier_image=$MEMTIER_IMAGE" \
    "size=$UC_MODE" "mode=$UC_MODE" "#reps=$UC_REPS" "cases=$UC_CASES_RUN" "cache_accounting=$UC_CACHE_ACCOUNTING" \
    "fio_fsync_status=${FIO_FSYNC_STATUS:-not run}" "#warmup_s=$UC_WARMUP" "#duration_s=$UC_DURATION" \
    "server_cpus=$SERVER_CPUS" "server_cores=$(server_cores)" "server_mem=$SERVER_MEM" "#server_nofile=$SERVER_NOFILE" \
    "#threads=$UC_THREADS" "#clients=$UC_CLIENTS" "client_cpus=$CLIENT_CPUS" "memtier_run_flags=${MEMTIER_RUN_FLAGS[*]}" \
    "#quiesce_window_s=$QUIESCE_WINDOW" "#quiesce_cpu_pct=$QUIESCE_CPU_PCT" \
    "#quiesce_write_bytes=$QUIESCE_WRITE_BYTES" "#quiesce_max_s=${QUIESCE_MAX:-600}" \
    "host=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs), kernel $(uname -r)" "started_utc=${TS:-}" <<'PY'
import json, sys
meta = {}
for arg in sys.argv[2:]:
    key, value = arg.split("=", 1)
    meta[key.lstrip("#")] = int(value) if key.startswith("#") else value
with open(sys.argv[1], "w") as f:
    json.dump(meta, f, indent=2)
PY
}
