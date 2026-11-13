#!/usr/bin/env bash
# Tests for the case functions in uc_cases_run.sh and the two-pass dispatch in uc_run.sh (registered in ctest as
# bench_uc_cases_run). Docker is stubbed: every server and memtier call is recorded as a "server:what" event.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source-path=SCRIPTDIR source=uc_run.sh
source "$HERE/uc_run.sh"
# shellcheck source-path=SCRIPTDIR source=uc_cases_run.sh
source "$HERE/uc_cases_run.sh"
fail=0
check() { if [[ $2 != "$3" ]]; then echo "FAIL $1: got [$2] want [$3]"; fail=1; else echo "ok   $1"; fi; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

EV=() START_LEVELS=() MT_LOG=() MT_CNAMES=() MT_FAIL="" DBSIZE_REPLY=999999999
# shellcheck disable=SC2034  # read by the memtier stub through ${!var}
STUB_OPS_tideskv=100000 STUB_OPS_kvrocks=80000
# shellcheck disable=SC2317  # the stubs are invoked indirectly by the code under test
{
start_server() { EV+=("$1:start") START_LEVELS+=("$1:$2"); SERVER_NAME=$1 SERVER_CONTAINER=c-$1; mkdir -p "$3"; }
parity_check() { return 0; }
stop_server() { SERVER_CONTAINER=""; }
remove_data() { EV+=("remove_data"); }
settle() { echo 0 >"$1"; }
memtier() {
  local name=$1 short var=STUB_OPS_${CUR//-/_}; shift
  short=${name#*/rep*/}
  EV+=("$CUR:$short") MT_LOG+=("$CUR:$short $*") MT_CNAMES+=("$short=${MEMTIER_CNAME:-}")
  mkdir -p "$(dirname "$OUT/$name")"
  printf '{"ALL STATS":{"Totals":{"Ops/sec":%s}}}\n' "${!var:-1}" >"$OUT/$name.json"
  [[ " $MT_FAIL " != *" $CUR:$short "* ]]
}
cli() {
  case $* in
    "DBSIZE SCAN") if [[ -n ${SCAN_TRACK:-} ]]; then echo "${SCAN_DELAY:-0}" >"$TMP/scanwait"; fi; echo OK ;;  # the scan lands after SCAN_DELAY INFO polls
    "INFO keyspace")
      if [[ -e $TMP/scanwait ]]; then
        w=$(<"$TMP/scanwait")
        if ((w > 0)); then echo $((w - 1)) >"$TMP/scanwait"; else rm -f "$TMP/scanwait"; echo $(($(cat "$TMP/scants" 2>/dev/null || echo 0) + 1)) >"$TMP/scants"; fi
      fi
      echo "last_dbsize_scan_timestamp:$(cat "$TMP/scants" 2>/dev/null || echo 0)" ;;
    DBSIZE) if [[ -e $TMP/scanwait ]]; then echo 0; else echo "$DBSIZE_REPLY"; fi ;;  # cached 0 until the scan lands
    INFO*) echo "is_compacting:no" ;;
    COMPACT) echo "$CUR:compact" >>"$TMP/compacts"; echo OK ;;  # cli runs in $(...): log to a file
    *) echo OK ;;
  esac
}
docker() { case $* in *State.Running*) echo true ;; *State.OOMKilled*) echo false ;; esac; }
cg_sample() { echo "1 1"; }
stats_sampler() { :; }
}
TS="test" DISK_FREE_OVERRIDE=$((1 << 50))
# number of the named files that exist under DIR
count_files() { local d=$1 n=0 f; shift; for f in "$@"; do [[ -e $d/$f ]] && n=$((n + 1)); done; echo "$n"; }

# Events without data-dir wipes and warm-ups, load parts collapsed ("load/3" -> "load").
evs() {
  printf '%s\n' "${EV[@]}" | grep -v -e '^remove_data$' -e '\.warmup$' | sed -E 's#/[0-9]+$##' | tr '\n' ' ' | sed 's/ $//'
}
pass_a_u3() { echo "$1:start $1:load $1:age $1:closed"; }
pass_b_u3() { echo "$1:start $1:reload $1:reload-age $1:open25 $1:open50 $1:open75"; }

# ---- open-loop rates: the lower closed max of the same repetition, per connection ----
OUT=$TMP/rates
uc_sizes full
for s in tideskv kvrocks; do mkdir -p "$OUT/U1/$s/rep1"; done
printf '{"ALL STATS":{"Totals":{"Ops/sec":100000}}}' >"$OUT/U1/tideskv/rep1/closed-gauss.json"
printf '{"ALL STATS":{"Totals":{"Ops/sec":80000.5}}}' >"$OUT/U1/kvrocks/rep1/closed-gauss.json"
printf '{"ALL STATS":{"Totals":{"Ops/sec":1000}}}' >"$OUT/U1/kvrocks/rep1/closed-uniform.json"
check open-rates "$(uc_open_rates U1 1)" "100 200 300"
check open-rates-missing "$(uc_open_rates U1 2 2>/dev/null || echo rc=$?)" "rc=1"
printf '{"ALL STATS":{"Totals":{"Ops/sec":0}}}' >"$OUT/U1/kvrocks/rep1/closed-gauss.json"
check open-rates-zero "$(uc_open_rates U1 1 2>/dev/null || echo rc=$?)" "rc=1"
printf '{"ALL STATS":{"Totals":{"Ops/sec":100}}}' >"$OUT/U1/kvrocks/rep1/closed-gauss.json"
check open-rates-floor-1 "$(uc_open_rates U1 1)" "1 1 1"

# ---- U3 full: pass A (load, age, closed) per server, then pass B (open25/50/75) per server ----
OUT=$TMP/full UC_MODE=full
uc_sizes full
UC_REPS=1 UC_WARMUP=0 U1_KEYS=1000 U3_KEYS=1000
EV=() MT_LOG=()
uc_dispatch U3 2>/dev/null
check u3-order "$(evs)" "$(pass_a_u3 tideskv) $(pass_a_u3 kvrocks) $(pass_b_u3 tideskv) $(pass_b_u3 kvrocks)"
check u3-done "$(cat "$OUT/U3/tideskv/rep1/done") $(cat "$OUT/U3/kvrocks/rep1/done")" "ok ok"
check u3-closed-done "$(cat "$OUT/U3/kvrocks/rep1/closed.done")" ok
check u3-rates-file "$(cat "$OUT/U3/kvrocks/rep1/open.rates")" "100 200 300"
check u3-open-rate-arg "$(printf '%s\n' "${MT_LOG[@]}" | grep -c '^tideskv:open50 .*--rate-limiting=200')" 1
check u3-age-du "$(head -1 "$OUT/U3/tideskv/rep1/du-after-age")" "epoch_s,bytes,wal_archive_bytes,allocated_bytes,wal_archive_allocated_bytes"
check u3-files "$(count_files "$OUT/U3/tideskv/rep1" load.seconds reload.seconds settle-load settle-age du-after-load \
  load reload age.json reload-age.json age.seconds)" 10
check u3-closed-seconds "$(printf '%s\n' "${MT_LOG[@]}" | grep '^kvrocks:closed ' | grep -o -- '--test-time=[0-9]*')" \
  "--test-time=$UC_U3_CLOSED_S"
check u3-open-seconds "$(printf '%s\n' "${MT_LOG[@]}" | grep '^kvrocks:open75 ' | grep -o -- '--test-time=[0-9]*')" \
  "--test-time=$UC_U3_OPEN_S"

# resume: a missing done with pass A kept reruns pass B of that server only, on a clean pass-B slate
rm "$OUT/U3/kvrocks/rep1/done"; touch "$OUT/U3/kvrocks/rep1/open-stale"
EV=()
uc_dispatch U3 2>/dev/null
check resume-pass-b-only "$(evs)" "$(pass_b_u3 kvrocks)"
check resume-pass-b-clean "$([[ -e $OUT/U3/kvrocks/rep1/open-stale ]] && echo stale || echo clean)" clean
check resume-pass-a-kept "$([[ -e $OUT/U3/kvrocks/rep1/closed.json ]] && echo kept)" kept
# a rerun pass A changes the rates, so every server's pass B of that repetition reruns
rm "$OUT/U3/kvrocks/rep1/closed.done"
EV=()
uc_dispatch U3 2>/dev/null
check resume-pass-a-reruns-b "$(evs)" "$(pass_a_u3 kvrocks) $(pass_b_u3 tideskv) $(pass_b_u3 kvrocks)"
check resume-all-done "$(cat "$OUT/U3/tideskv/rep1/done") $(cat "$OUT/U3/kvrocks/rep1/done")" "ok ok"
EV=()
uc_dispatch U3 2>/dev/null
check resume-nothing-left "$(evs)" ""

# a failed closed phase: no open phases on either server, both done files say why
OUT=$TMP/fail MT_FAIL="kvrocks:closed"
EV=()
uc_dispatch U3 2>/dev/null
check closed-fail-no-open "$(evs)" "$(pass_a_u3 tideskv) $(pass_a_u3 kvrocks)"
check closed-fail-done-k "$(cut -c1-18 "$OUT/U3/kvrocks/rep1/done")" "cell closed failed"
check closed-fail-done-t "$(cat "$OUT/U3/tideskv/rep1/done")" "open phases not run: kvrocks rep1 closed phases did not finish ok"
MT_FAIL=""

# ---- U1 full, then U8 from its files ----
OUT=$TMP/full
EV=()
rm -f "$TMP/compacts"
uc_dispatch U1 2>/dev/null
check u1-full-open "$(evs | grep -o 'tideskv:open[0-9]*' | tr '\n' ' ')" "tideskv:open25 tideskv:open50 tideskv:open75 "
check u1-reload-seconds "$([[ -s $OUT/U1/kvrocks/rep1/reload.seconds ]] && echo y)" y
check u1-compact-kvrocks "$(cat "$TMP/compacts")" "kvrocks:compact"
check u1-du-after-compact "$([[ -e $OUT/U1/kvrocks/rep1/du-after-compact && ! -e $OUT/U1/tideskv/rep1/du-after-compact ]] && echo y)" y
check u1-cell-info "$(cat "$OUT/U1/kvrocks/rep1/closed-gauss.info0") $(cat "$OUT/U1/tideskv/rep1/closed-gauss.info1")" \
  "is_compacting:no is_compacting:no"
check u1-client-cname "$(printf '%s\n' "${MT_CNAMES[@]}" | grep '^closed-uniform=' | grep -c -- "-$TS-")" 2
check u1-client-sampler "$([[ -e $OUT/U1/tideskv/rep1/closed-gauss.client.jsonl ]] && echo y)" y

for s in tideskv kvrocks; do echo 4 >"$OUT/U1/$s/rep1/load.seconds"; done
EV=()
uc_dispatch U8 2>/dev/null
check u8-no-server "$(evs)" ""
check u8-done "$(cat "$OUT/U8/kvrocks/rep1/done")" ok
check u8-summary "$(python3 -c "
import json
t = json.load(open('$OUT/U8/tideskv/rep1/summary.json'))
k = json.load(open('$OUT/U8/kvrocks/rep1/summary.json'))
print(t['load_ops_per_s'], t['load_keys'], t['settle_load_s'], t['kvrocks_after_compact'], k['kvrocks_after_compact'] is not None,
      sorted(t['du_after_age']), t['du_after_load']['bytes'] >= 0)")" "250 1000 0 None True ['allocated_bytes', 'bytes', 'wal_archive_allocated_bytes', 'wal_archive_bytes'] True"
rm -rf "$OUT/U8"
mv "$OUT/U3" "$OUT/U3.away"
uc_dispatch U8 2>/dev/null
check u8-needs-u3 "$(cat "$OUT/U8/tideskv/rep1/done")" "U8 needs U3 rep1 of tideskv (run U3 first)"
mv "$OUT/U3.away" "$OUT/U3"

# ---- quick: closed phases only, one pass ----
OUT=$TMP/quick UC_MODE=quick
uc_sizes quick
UC_WARMUP=0 U1_KEYS=1000 U4_KEYS=10
EV=()
rm -f "$TMP/compacts"
uc_dispatch U1 2>/dev/null
check quick-no-open "$(evs)" "tideskv:start tideskv:load tideskv:closed-gauss tideskv:closed-uniform kvrocks:start kvrocks:load kvrocks:closed-gauss kvrocks:closed-uniform"
check quick-compact-kvrocks-only "$(tr '\n' ' ' <"$TMP/compacts")" "kvrocks:compact "
check quick-done "$(cat "$OUT/U1/tideskv/rep1/done") $([[ -e $OUT/U1/tideskv/rep1/closed.done ]] && echo two-pass || echo one-pass)" "ok one-pass"

# ---- U5: each size on its own data dir, removed after the size ----
EV=() CUR=tideskv
case_U5 tideskv 1
check u5-removes-between-sizes "$(printf '%s\n' "${EV[@]}" | sed -E 's#/[0-9]+$##' | grep -v '\.warmup$' | tr '\n' ' ')" \
  "tideskv:start tideskv:load-v1k tideskv:v1k remove_data tideskv:start tideskv:load-v4k tideskv:v4k remove_data tideskv:start tideskv:load-v16k tideskv:v16k remove_data "
check u5-files "$(count_files "$OUT/U5/tideskv/rep1" load-v4k.seconds settle-load-v4k du-after-load-v4k v4k.json)" 4

# ---- U4 and the load check ----
EV=()
uc_dispatch U4 2>/dev/null
check u4-order "$(evs)" "tideskv:start tideskv:load tideskv:mix kvrocks:start kvrocks:load kvrocks:mix"
DBSIZE_REPLY=$((U4_KEYS * 99 / 100))
rc=0; case_U4 tideskv 8 2>/dev/null || rc=$?
check load-check-99pct "$rc" 0
DBSIZE_REPLY=$((U4_KEYS * 99 / 100 - 1)) UC_FAIL_REASON=""
rc=0; case_U4 tideskv 9 2>/dev/null || rc=$?
check load-check-fails "$rc ${UC_FAIL_REASON:0:17}" "1 load check failed"
DBSIZE_REPLY=999999999

# ---- U2: the du sampler spans churn and idle; U6: strict start, cells in order ----
OUT=$TMP/u2 UC_MODE=quick
uc_sizes quick
UC_U2_WRITE_S=7 UC_U2_IDLE_S=1 UC_U2_DU_EVERY=1 U6_KEYS=1000 UC_U6_S=4
U2_LOG=$TMP/u2.ev
# shellcheck disable=SC2317
{
uc_du_sampler() {
  echo sampler-start >>"$U2_LOG"
  echo "epoch_s,bytes,wal_archive_bytes" >"$3"
  trap 'echo sampler-stopped >>"$U2_LOG"; exit 0' TERM
  while :; do sleep 0.2; done
}
memtier() { if [[ $1 == */churn ]]; then echo "churn-$CUR" >>"$U2_LOG"; echo "args $*" >>"$U2_LOG"; fi; EV+=("$CUR:${1#*/rep*/}"); mkdir -p "$(dirname "$OUT/$1")"; printf '{"ALL STATS":{"Totals":{"Ops/sec":1}}}\n' >"$OUT/$1.json"; }
sleep() { [[ ${1:-} == 1 && -n ${U2_IDLE_MARK:-} ]] && echo idle >>"$U2_LOG"; command sleep "$@"; }
}
UC_DBSIZE_TRIES=3 U2_IDLE_MARK=1 SCAN_TRACK=1
: >"$U2_LOG"
EV=()
uc_dispatch U2 2>/dev/null
check u2-three-configs "$(evs)" "tideskv:start tideskv:churn kvrocks:start kvrocks:churn kvrocks-cron:start kvrocks-cron:churn"
check u2-sampler-spans-idle "$(grep -v '^args' "$U2_LOG" | tr '\n' ' ')" \
  "sampler-start churn-tideskv idle sampler-stopped sampler-start churn-kvrocks idle sampler-stopped sampler-start churn-kvrocks-cron idle sampler-stopped "
check u2-churn-seconds "$(grep -m1 '^args' "$U2_LOG" | grep -o -- '--test-time=[0-9]*')" "--test-time=7"
check u2-files "$(count_files "$OUT/U2/kvrocks-cron/rep1" du.csv du-end dbsize-end churn.json churn.status)" 5
check u2-done "$(cat "$OUT/U2/tideskv/rep1/done") $(cat "$OUT/U2/kvrocks/rep1/done")" "ok ok"
check u2-dbsize-recorded "$(cat "$OUT/U2/kvrocks/rep1/dbsize-end") $(cat "$OUT/U2/tideskv/rep1/dbsize-end")" "999999999 999999999"
check u2-value-size "$(cat "$OUT/U2/kvrocks/rep1/value-size")" 1024
unset -f sleep
SCAN_TRACK=""; rm -f "$TMP/scanwait"
EV=() START_LEVELS=() FIO_DIRS=()
OUT=$TMP/u6
# R24: the fsync floor is measured before each server's sweep, into that server's rep dir
# shellcheck disable=SC2317
run_fio_fsync() { FIO_DIRS+=("${1#"$OUT"/}"); EV+=("$CUR:fio"); }
uc_dispatch U6 2>/dev/null
check u6-cells "$(evs)" "tideskv:start tideskv:load tideskv:fio tideskv:c1 tideskv:c8 tideskv:c64 tideskv:c256 kvrocks:start kvrocks:load kvrocks:fio kvrocks:c1 kvrocks:c8 kvrocks:c64 kvrocks:c256"
check u6-fio-dirs "${FIO_DIRS[*]}" "U6/tideskv/rep1/fio U6/kvrocks/rep1/fio"
check u6-strict "${START_LEVELS[*]}" "tideskv:strict kvrocks:strict"
check u6-done "$(cat "$OUT/U6/tideskv/rep1/done")" ok
check u6-load-pipeline-recorded "$(cat "$OUT/U6/tideskv/rep1/load.pipeline")" 32

SCAN_TRACK=1; rm -f "$TMP/scanwait"
# Kvrocks' DBSIZE is a cached 0 until the scan lands: wait for it, and never record the false 0
SERVER_NAME=kvrocks DBSIZE_REPLY=424242 UC_DBSIZE_POLL=0.01 UC_DBSIZE_TRIES=50 SCAN_DELAY=3
mkdir -p "$OUT/dbs"; rm -f "$TMP/scanwait"
_uc_dbsize_end dbs
check dbsize-end-waits-for-scan "$(cat "$OUT/dbs/dbsize-end")" 424242
SCAN_DELAY=1000 UC_DBSIZE_TRIES=4
_uc_dbsize_end dbs
check dbsize-end-timeout-unverified "$(cat "$OUT/dbs/dbsize-end")" unverified
SERVER_NAME=tideskv; rm -f "$TMP/scanwait"; DBSIZE_REPLY=0
_uc_dbsize_end dbs
check dbsize-end-tideskv-direct "$(cat "$OUT/dbs/dbsize-end")" 0
DBSIZE_REPLY=999999999

# ---- U7: per kill point = baseline probe, writer, signal, restart, probe, verify (strict, both servers) ----
OUT=$TMP/u7 UC_MODE=quick
uc_sizes quick
U7_KEYS=1000 UC_U7_ACKS="20 50" UC_U7_TIMES=1 SERVER_PORT=6379 NET=testnet SCAN_TRACK="" DBSIZE_REPLY=999999999
rm -f "$TMP/scanwait"
# shellcheck disable=SC2317
{
start_server() {
  EV+=("$CUR:start${START_NOWAIT:+-nowait}${START_T0_FILE:+-t0file}${START_RUN_MS_FILE:+-runms}") START_LEVELS+=("$1:$2"); SERVER_NAME=$1 SERVER_CONTAINER=c-$1; mkdir -p "$3"
  EVICT_FAILED=${STUB_EVICT:-0}
}
stop_server() { EV+=("$CUR:stop"); SERVER_CONTAINER=""; }
sleep() { EV+=("$CUR:sleep-$1"); }
_uc_u7_wait_writing() { EV+=("$CUR:writing"); }
# the writer "acks" U7_STUB_ACKS keys at once (one line each in ack.0, as the real one logs them)
_uc_u7_writer_start() { EV+=("$CUR:writer") PREFIXES+=("$3"); seq "${U7_STUB_ACKS:-100}" >"$OUT/$1/ack.0"; }
_uc_u7_probe_start() { EV+=("$CUR:probe-up"); }
_uc_u7_wait_exit() { EV+=("$CUR:exit-$1"); }
docker() {
  case $* in
    "kill -s "*) EV+=("$CUR:signal-${3}") ;;
    *ExitCode*) echo "${WRITER_RC:-0}" ;;
    "logs "*) echo "acked 42" ;;
    "rm "*) EV+=("$CUR:rm") ;;
    *State.Running*) echo true ;;
  esac
}
_uc_py() {
  local dir=$1 mode=$2; shift 2
  EV+=("$CUR:$mode") PY_ARGS+=("$mode $*")
  case $mode in
    probe) if [[ $* == *--baseline-secs* ]]; then echo '{"p99_ms": 0.75}' >"$OUT/$dir/baseline.json"; fi ;;
    verify) echo '{"acked": 42, "lost": 0, "lost_sample": []}' >"$OUT/$dir/verify.json" ;;
  esac
}
}
EV=() START_LEVELS=() PY_ARGS=() PREFIXES=()
uc_dispatch U7 2>/dev/null
check u7-done "$(cat "$OUT/U7/tideskv/rep1/done") $(cat "$OUT/U7/kvrocks/rep1/done")" "ok ok"
check u7-strict "${START_LEVELS[*]}" "tideskv:strict tideskv:strict tideskv:strict tideskv:strict tideskv:strict kvrocks:strict kvrocks:strict kvrocks:strict kvrocks:strict kvrocks:strict"
seq_of() { printf '%s\n' "${EV[@]}" | grep "^$1:" | grep -v -e ':load' -e 'exit-tideskv-bench' | sed 's/^[^:]*://' | tr '\n' ' '; }
check u7-sequence-tideskv "$(seq_of tideskv)" \
  "start probe probe-up writer writing signal-TERM exit-c-tideskv rm stop start-nowait-t0file-runms rm verify probe probe-up writer writing signal-KILL exit-c-tideskv rm stop start-nowait-t0file-runms rm verify probe probe-up writer writing signal-TERM exit-c-tideskv rm stop start-nowait-t0file-runms rm verify probe probe-up writer writing signal-KILL exit-c-tideskv rm stop start-nowait-t0file-runms rm verify stop "
# F3: the signal goes once N writes are acked; the count and the target are recorded, the same N on both servers
check u7-acked-files "$(cat "$OUT/U7/kvrocks/rep1/kill-50/acked-target" "$OUT/U7/kvrocks/rep1/kill-50/acked-at-signal" "$OUT/U7/kvrocks/rep1/kill-50/ack.status" | tr '\n' ' ')" "50 100 ok "
check u7-same-n "$(cat "$OUT/U7/tideskv/rep1/term-20/acked-target") $(cat "$OUT/U7/kvrocks/rep1/term-20/acked-target")" "20 20"
check u7-verify-value-size "$(printf '%s\n' "${PY_ARGS[@]}" | grep -m1 '^verify ' | grep -o -- '--value-size 2048')" "--value-size 2048"
check u7-no-evict-note "$(find "$OUT/U7" -name evict-failed | wc -l)" 0
check u7-point-files "$(count_files "$OUT/U7/kvrocks/rep1/kill-50" baseline.json until-p99-ms verify.json signal.epoch_ms writer.out writer.status probe.status)" 7
check u7-term-seconds "$([[ -s $OUT/U7/kvrocks/rep1/term-20/term.seconds && ! -e $OUT/U7/kvrocks/rep1/kill-20/term.seconds ]] && echo y)" y
check u7-until-is-2x-baseline "$(cat "$OUT/U7/tideskv/rep1/term-20/until-p99-ms")" "1.5"
check u7-baseline-args "$(printf '%s\n' "${PY_ARGS[@]}" | grep -m1 '^probe ' | grep -o -e '--baseline-secs 10' -e '--prefix u7:' | tr '\n' ' ')" \
  "--baseline-secs 10 --prefix u7: "
check u7-unique-prefixes "${PREFIXES[*]:0:4}" "ack-r1-term-20-1- ack-r1-kill-20-1- ack-r1-term-50-1- ack-r1-kill-50-1-"
check u7-prefixes-distinct "$(printf '%s\n' "${PREFIXES[@]:0:4}" | sort | uniq -d | wc -l)" 0
check u7-load-pipeline "$(cat "$OUT/U7/tideskv/rep1/load.pipeline")" 32
# repeats get a run<i> sub-directory
OUT=$TMP/u7t UC_U7_ACKS="5" UC_U7_TIMES=2
EV=() PREFIXES=()
uc_dispatch U7 2>/dev/null
check u7-times-prefixes "${PREFIXES[*]:0:4}" "ack-r1-term-5-1- ack-r1-term-5-2- ack-r1-kill-5-1- ack-r1-kill-5-2-"
check u7-times-dirs "$(cd "$OUT/U7/tideskv/rep1" && printf '%s ' term-5/run* kill-5/run*)" "term-5/run1 term-5/run2 kill-5/run1 kill-5/run2 "
# N not reached within the timeout: the point says why (ERR in the report), is still signalled and verified, the rep goes on
OUT=$TMP/u7slow UC_U7_ACKS="30" UC_U7_TIMES=1 U7_STUB_ACKS=7 UC_U7_ACK_TIMEOUT=0 STUB_EVICT=1
EV=()
uc_dispatch U7 2>/dev/null
check u7-timeout-status "$(cat "$OUT/U7/tideskv/rep1/term-30/ack.status")" "load point not reached: 7 of 30 acknowledged writes in 0 s"
check u7-timeout-signalled "$(cat "$OUT/U7/tideskv/rep1/term-30/acked-at-signal") $([[ -e $OUT/U7/tideskv/rep1/term-30/verify.json ]] && echo verified) $(cat "$OUT/U7/tideskv/rep1/done")" "7 verified ok"
# R-minor 10: a failed page-cache eviction is noted in the rep dir (first start) and in the point dir (restart)
check u7-evict-noted "$(cat "$OUT/U7/kvrocks/rep1/evict-failed") / $(cat "$OUT/U7/kvrocks/rep1/term-30/evict-failed")" \
  "start: could not drop the data dir from the page cache / restart: could not drop the data dir from the page cache"
U7_STUB_ACKS=100 STUB_EVICT=0 UC_U7_ACK_TIMEOUT=300
# a failing writer fails the cell and says why
OUT=$TMP/u7f UC_U7_TIMES=1 WRITER_RC=2
uc_dispatch U7 2>/dev/null
check u7-writer-fail "$(cut -c1-21 "$OUT/U7/tideskv/rep1/done")" "the writer exited 2: "
WRITER_RC=0
unset -f sleep

# ---- F1: Redis 7.2 reference started the tracked way, the tool run per server, matrices merged ----
OUT=$TMP/f1 F1_LOG=$TMP/f1.log
: >"$F1_LOG"
# shellcheck disable=SC2317
{
docker() {
  echo "docker $*" >>"$F1_LOG"
  case $* in
    *"redis-cli PING"*) echo PONG ;;
    *State.Running*) echo true ;;
  esac
  return 0
}
_uc_f1_tool() {  # writes the per-server matrix the real tool would
  local s=${4%%=*}
  echo "tool $*" >>"$F1_LOG"
  [[ $F1_TOOL_RC == 0 ]] && python3 - "$s" >"$OUT/$1/matrix.json" <<'PY'
import json, sys
s = sys.argv[1]
print(json.dumps({"ref": "r", "targets": [s], "cases": [{"family": "strings", "case": "c", "ref_accepts": True,
                                                       "results": {s: {"status": "same"}}}]}))
PY
  echo "boom line" >&2
  return "$F1_TOOL_RC"
}
}
F1_TOOL_RC=0
uc_dispatch F1 2>/dev/null
check f1-done "$(cat "$OUT/F1/tideskv/rep1/done") $(cat "$OUT/F1/kvrocks/rep1/done")" "ok ok"
check f1-redis-no-persistence "$(grep -c 'run -d --name tideskv-bench-redis-test-.* redis-server --save  --appendonly no' "$F1_LOG")" 2
check f1-redis-removed "$(grep -c 'rm -f tideskv-bench-redis-test-' "$F1_LOG")" 2
check f1-tool-args "$(grep -c -e '--target tideskv=c-tideskv:6379 --ref tideskv-bench-redis-test-[0-9]*:6379 --cases /f1/f1_cases.txt' "$F1_LOG")" 1
check f1-merged-targets "$(python3 -c 'import json,sys; print(" ".join(json.load(open(sys.argv[1]))["targets"]))' "$OUT/F1/matrix.json")" "kvrocks tideskv"
check f1-levels "$(printf '%s ' "${START_LEVELS[@]}" | grep -c 'tideskv:relaxed kvrocks:relaxed')" 1
OUT=$TMP/f1f F1_TOOL_RC=1
uc_dispatch F1 2>/dev/null
check f1-tool-fail "$(cat "$OUT/F1/tideskv/rep1/done")" "f1_matrix.py failed: boom line"
check f1-ref-removed-on-failure "$(grep -c 'rm -f tideskv-bench-redis-test-' "$F1_LOG")" 4
unset -f docker _uc_f1_tool

# ---- start_server's restart timestamps: t0 after docker run, with docker run's duration recorded apart ----
(
  source "$HERE/lib.sh"
  OUT=$TMP/t0 TS=t SERVER_CPUS=0 SERVER_MEM=1g TIDESKV_IMAGE=img
  mkdir -p "$OUT"
  evict_cache() { :; }; write_server_conf() { echo "$TMP/conf"; }; _track() { :; }; log() { :; }
  docker() { if [[ $1 == run ]]; then sleep 0.3; fi; }
  before=$(date +%s%3N)
  START_NOWAIT=1 START_T0_FILE=$OUT/t0 START_RUN_MS_FILE=$OUT/run-ms start_server tideskv strict "$OUT/data"
  after=$(date +%s%3N)
  t0=$(<"$OUT/t0") run=$(<"$OUT/run-ms")
  ok_t0=n; ((t0 >= before + 300 && t0 <= after)) && ok_t0=y
  ok_run=n; ((run >= 300 && run < 1000)) && ok_run=y
  echo "$ok_t0 $ok_run" >"$OUT/result"
)
check start-t0-after-docker-run "$(cat "$TMP/t0/result")" "y y"

# ---- shellcheck ----
if command -v shellcheck >/dev/null; then
  if (cd "$HERE" && shellcheck -x uc_cases_run.sh test_uc_cases_run.sh); then check shellcheck ok ok; else check shellcheck fail ok; fi
fi
exit $fail
