#!/usr/bin/env bash
# Tests for uc_run.sh and usecases.sh (registered in ctest as bench_uc_run). Docker is stubbed.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source-path=SCRIPTDIR source=uc_run.sh
source "$HERE/uc_run.sh"
fail=0
check() { if [[ $2 != "$3" ]]; then echo "FAIL $1: got [$2] want [$3]"; fail=1; else echo "ok   $1"; fi; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# ---- stubs: nothing touches Docker ----
CALLS=()
# shellcheck disable=SC2317  # the stubs are invoked indirectly by the code under test
{
settle() { CALLS+=("settle"); echo 0 >"$1"; }
remove_data() { CALLS+=("remove_data"); }
memtier() { CALLS+=("memtier $*"); mkdir -p "$(dirname "$OUT/$1")"; echo '{}' >"$OUT/$1.json"; }
cli() { CALLS+=("cli $*"); }
DOCKER_RUNNING=true
docker() {
  CALLS+=("docker $*")
  case $* in
    *State.Running*) echo "$DOCKER_RUNNING" ;;
    *State.OOMKilled*) echo true ;;
  esac
}
cg_sample() { echo "1000 2000"; }
stop_server() { :; }
}
OUT=$TMP/run TS=test SERVER_CONTAINER=none CUR=kvrocks UC_REPS=3

uc_sizes full
check rep-order-full "$(uc_rep_order U1 | tr '\n' ',')" "1 tideskv,1 kvrocks,2 tideskv,2 kvrocks,3 tideskv,3 kvrocks,"
check rep-order-u2 "$(uc_rep_order U2 | head -3 | tr '\n' ',')" "1 tideskv,1 kvrocks,1 kvrocks-cron,"
UC_REPS=1
check rep-order-quick "$(uc_rep_order U1 | wc -l)" 2
UC_REPS=2

# ---- dispatcher ----
mkdir -p "$OUT/U1/tideskv/rep1" "$OUT/U1/kvrocks/rep1"
echo ok >"$OUT/U1/tideskv/rep1/done"
echo stale >"$OUT/U1/kvrocks/rep1/leftover"
check is-done-yes "$(uc_is_done U1 tideskv 1 && echo y || echo n)" y
check is-done-no "$(uc_is_done U1 kvrocks 1 && echo y || echo n)" n
RAN=()
# shellcheck disable=SC2317
case_U1() { RAN+=("$1 $2"); [[ -e $OUT/U1/$1/rep$2/leftover ]] && RAN+=("stale-left"); return 0; }
DISK_FREE_OVERRIDE=$((1 << 50))
uc_dispatch U1 2>/dev/null
check resume-skips-done-only "${RAN[*]}" "kvrocks 1 tideskv 2 kvrocks 2"
check resume-ok-written "$(cat "$OUT/U1/kvrocks/rep1/done")" ok
check resume-empties-data "$(printf '%s\n' "${CALLS[@]}" | grep -c '^remove_data$' | awk '{print ($1>=1)?"yes":"no"}')" yes
check resume-keeps-done "$(cat "$OUT/U1/tideskv/rep1/done")" ok

# failing case: done carries the reason, never ok
# shellcheck disable=SC2317
case_U4() { UC_FAIL_REASON="server did not start"; return 1; }
uc_dispatch U4 2>/dev/null
check done-written-last-fail "$(cat "$OUT/U4/tideskv/rep1/done")" "server did not start"
# shellcheck disable=SC2317
case_U5() { return 7; }
uc_dispatch U5 2>/dev/null
check done-fail-default-reason "$(cat "$OUT/U5/tideskv/rep1/done")" "case_U5 exited 7"
# done does not exist while the case runs
# shellcheck disable=SC2317
case_U6() { [[ ! -e $OUT/U6/$1/rep$2/done ]] && echo "$1" >>"$TMP/seen"; }
uc_dispatch U6 2>/dev/null
check no-done-while-running "$(sort -u "$TMP/seen" | tr '\n' ' ')" "kvrocks tideskv "

# R2: undefined case function -> skipped, no done file
log_out=$(uc_dispatch U8 2>&1)
check unimplemented-logged "$([[ $log_out == *"case U8 not implemented, skipping"* ]] && echo y)" y
check unimplemented-no-done "$(find "$OUT/U8" -name "done" 2>/dev/null | wc -l)" 0

# disk check per case: the case is skipped (done reason), the run goes on
DISK_FREE_OVERRIDE=1024
RAN=()
uc_dispatch U1 2>/dev/null || true
rm -rf "$OUT/U7"
# shellcheck disable=SC2317
case_U7() { RAN+=("U7"); }
uc_dispatch U7 2>/dev/null
check disk-skip-not-run "${RAN[*]:-}" ""
check disk-skip-reason "$(cut -c1-9 "$OUT/U7/tideskv/rep1/done")" "skipped: "
check disk-skip-needs "$([[ $(cat "$OUT/U7/tideskv/rep1/done") == "skipped: needs "*" GiB free" ]] && echo y)" y
DISK_FREE_OVERRIDE=$((1 << 50))

# ---- uc_cell ----
CALLS=() UC_CELL_WARMUP=5 UC_CELL_DURATION=9
uc_cell "U1/kvrocks/rep1/closed" --ratio=1:0 --key-prefix=x
mt=$(printf '%s\n' "${CALLS[@]}" | grep '^memtier')
check cell-order "$(printf '%s\n' "${CALLS[@]}" | sed 's/ .*//' | tr '\n' ' ')" "settle memtier memtier "
check cell-warmup "$(sed -n 1p <<<"$mt")" "memtier U1/kvrocks/rep1/closed.warmup --ratio=1:0 --key-prefix=x --test-time=5"
check cell-timed "$(sed -n 2p <<<"$mt")" "memtier U1/kvrocks/rep1/closed --ratio=1:0 --key-prefix=x --test-time=9"
check cell-status "$(cat "$OUT/U1/kvrocks/rep1/closed.status")" ok
check cell-cpu "$(cat "$OUT/U1/kvrocks/rep1/closed.cpu") $(cat "$OUT/U1/kvrocks/rep1/closed.io")" "0 0"
CALLS=() UC_CELL_WARMUP=0
uc_cell "U1/kvrocks/rep1/nowarm" --ratio=1:0
check cell-no-warmup "$(printf '%s\n' "${CALLS[@]}" | grep -c '^memtier')" 1

uc_cell "U1/kvrocks/rep1/ok" --ratio=1:0 --test-time=1 && rc=0 || rc=$?
check cell-ok-returns-0 "$rc" 0
DOCKER_RUNNING=false
uc_cell "U1/kvrocks/rep1/oom" --ratio=1:0 && rc=0 || rc=$?
check cell-dead-server-rc "$rc" 1
check cell-dead-server-status "$(cat "$OUT/U1/kvrocks/rep1/oom.status")" "server stopped during the run (OOM-killed: true)"
DOCKER_RUNNING=true
# a failing memtier makes the cell fail
memtier() { CALLS+=("memtier $*"); mkdir -p "$(dirname "$OUT/$1")"; return 3; }
uc_cell "U1/kvrocks/rep1/mtfail" --ratio=1:0 && rc=0 || rc=$?
check cell-memtier-fail-rc "$rc" 1
check cell-memtier-fail-status "$(head -c 14 "$OUT/U1/kvrocks/rep1/mtfail.status")" "memtier exited"
# a case whose uc_cell fails ends with a done that is not ok
# shellcheck disable=SC2317
case_F1() { UC_CELL_WARMUP=0 UC_CELL_DURATION=1 uc_cell "F1/$1/rep$2/m" --ratio=1:0 || { UC_FAIL_REASON="cell m failed"; return 1; }; }
uc_dispatch F1 2>/dev/null
check case-cell-fail-done "$(cat "$OUT/F1/tideskv/rep1/done")" "cell m failed"
# ...and without a reason it is still not ok
# shellcheck disable=SC2317
case_F1() { uc_cell "F1/$1/rep$2/m" --ratio=1:0; }
rm -rf "$OUT/F1"; uc_dispatch F1 2>/dev/null
check case-cell-fail-done-default "$([[ $(cat "$OUT/F1/tideskv/rep1/done") != ok ]] && echo y)" y

# ---- du sampler ----
mkdir -p "$TMP/d/sub/archive" "$TMP/d/sub/other"
head -c 4096 /dev/zero >"$TMP/d/sub/archive/w"
head -c 8192 /dev/zero >"$TMP/d/sub/other/f"
uc_du_sampler "$TMP/d" 0.2 "$TMP/du.csv" & dpid=$!
sleep 0.7; kill "$dpid" 2>/dev/null || true; wait "$dpid" 2>/dev/null || true
check du-header "$(head -1 "$TMP/du.csv")" "epoch_s,bytes,wal_archive_bytes,allocated_bytes,wal_archive_allocated_bytes"
last=$(tail -1 "$TMP/du.csv")
# columns 1-3 keep their meaning (apparent size); the allocated twins are appended (R22)
check du-row "$([[ $last =~ ^[0-9]+,[0-9]+,4096,[0-9]+,[0-9]+$ ]] && echo y)" y
IFS=, read -r _ apparent _ alloc wal_alloc <<<"$last"
check du-allocated-is-blocks "$((alloc % 512 == 0 && alloc >= apparent && wal_alloc >= 4096 && wal_alloc <= alloc)) $(( ${#alloc} > 0 ))" "1 1"
mkdir -p "$TMP/t"
uc_du_sampler "$TMP/t" 0.2 "$TMP/du2.csv" & dpid=$!
sleep 0.5; kill "$dpid" 2>/dev/null || true; wait "$dpid" 2>/dev/null || true
check du-no-archive "$(tail -1 "$TMP/du2.csv" | cut -d, -f3)" 0

# ---- meta.json ----
OUT=$TMP/meta; mkdir -p "$OUT"
SERVER_CPUS=0 UC_CASES_RUN="U1 U2" UC_MODE=quick DIRTY=0 FIO_FSYNC_STATUS=ok
uc_sizes quick
uc_write_meta 2>/dev/null
check meta-fields "$(python3 -c "
import json,sys
m=json.load(open('$OUT/meta.json'))
print(m['mode'], m['reps'], m['cases'], m['cache_accounting'], m['fio_fsync_status'], 'size' in m, 'started_utc' in m,
      m['fio_append_status'], m['fio_status'], 'tideskv_image_id' in m)")" "quick 1 U1 U2 charged ok True True not run not run True"
# shellcheck disable=SC2016  # the pattern matches the literal script text
check usecases-runs-report "$(grep -c '^python3 "$HERE/uc_report.py" "$OUT" || rc=$?$' "$HERE/usecases.sh")" 1


# ---- F4: resume refuses a run directory written with other images or resources ----
OUT=$TMP/resume; mkdir -p "$OUT"
# shellcheck disable=SC2317
uc_identity() { printf '%s\n' "mode=quick" "tideskv_image=t:1" "tideskv_image_id=sha256:aa" "kvrocks_image=k@sha256:bb" \
  "server_cpus=0-3" "server_mem=8g" "client_cpus=8-15"; }
rc=0; uc_check_resume >/dev/null 2>&1 || rc=$?
check resume-no-meta "$rc" 0
mapfile -t ID < <(uc_identity)
write_meta_kv "$OUT/meta.json" "${ID[@]}" "started_utc=x"
rc=0; uc_check_resume >/dev/null 2>&1 || rc=$?
check resume-same "$rc" 0
mapfile -t ID2 < <(printf '%s\n' "${ID[@]}" | sed -e 's/^server_cpus=.*/server_cpus=0-7/' -e '/tideskv_image_id/d')
write_meta_kv "$OUT/meta.json" "${ID2[@]}"
rc=0; msg=$(uc_check_resume 2>&1) || rc=$?
check resume-mismatch-rc "$rc" 2
check resume-mismatch-says "$([[ $msg == *"server_cpus: run directory 0-7, this run 0-3"* ]] && echo y)" y
# an older meta.json without the image id is compared on the other fields only
check resume-mismatch-only-cpus "$(grep -c '^cannot resume' <<<"$msg")" 1
mapfile -t ID2 < <(printf '%s\n' "${ID[@]}" | sed -e 's/^mode=.*/mode=full/')
write_meta_kv "$OUT/meta.json" "${ID2[@]}"
rc=0; uc_check_resume >/dev/null 2>&1 || rc=$?
check resume-mode-mismatch "$rc" 2
unset -f uc_identity

# ---- image stamp per rep, remove_data failure, U8 invalidated by a redone U1/U3 ----
OUT=$TMP/stamp UC_REPS=1 UC_MODE=quick UC_IMAGE_STAMP=("tideskv_image=t:1" "kvrocks_image=k@sha256:bb")
# shellcheck disable=SC2317
case_U4() { return 0; }
uc_dispatch U4 2>/dev/null
check image-stamp "$(cat "$OUT/U4/kvrocks/rep1/images.done")" "$(printf '%s\n' "${UC_IMAGE_STAMP[@]}")"
# shellcheck disable=SC2317
remove_data() { CALLS+=("remove_data"); return 1; }
RAN=()
# shellcheck disable=SC2317
case_U2() { RAN+=("$1"); }
uc_dispatch U2 2>/dev/null
check remove-data-fail-not-run "${RAN[*]:-}" ""
check remove-data-fail-done "$(cat "$OUT/U2/tideskv/rep1/done")" "could not empty $OUT/data before the repetition"
# shellcheck disable=SC2317
remove_data() { CALLS+=("remove_data"); }
mkdir -p "$OUT/U8/tideskv/rep1" "$OUT/U8/kvrocks/rep1"
echo ok >"$OUT/U8/tideskv/rep1/done"; echo ok >"$OUT/U8/kvrocks/rep1/done"
# shellcheck disable=SC2317
case_U3() { return 0; }
mkdir -p "$OUT/U3/kvrocks/rep1"; echo ok >"$OUT/U3/kvrocks/rep1/done"  # only TidesKV's U3 is redone
uc_dispatch U3 2>/dev/null
check u8-invalidated "$([[ -e $OUT/U8/tideskv/rep1/done ]] && echo kept || echo gone) $([[ -e $OUT/U8/kvrocks/rep1/done ]] && echo kept || echo gone)" "gone kept"

# ---- usecases.sh CLI (no Docker reached: argument errors exit before bench_init) ----
rc=0; bash "$HERE/usecases.sh" NOPE >/dev/null 2>&1 || rc=$?
check args-unknown-case "$rc" 2
rc=0; bash "$HERE/usecases.sh" --bogus >/dev/null 2>&1 || rc=$?
check args-unknown-option "$rc" 2
check help-text "$(bash "$HERE/usecases.sh" --help | head -1 | cut -c1-30)" "# Use-case suite runner."

# ---- resolve_out allows a non-empty data/ (resume) ----
mkdir -p "$TMP/res/data" && touch "$TMP/res/data/x"
check resolve-resume "$(resolve_out "$TMP/res" 2>/dev/null)" "$TMP/res"

# ---- shellcheck ----
if command -v shellcheck >/dev/null; then
  if (cd "$HERE" && shellcheck -x uc_run.sh usecases.sh test_uc_run.sh lib.sh run.sh); then check shellcheck ok ok; else check shellcheck fail ok; fi
fi
exit $fail
