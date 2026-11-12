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
check u3-age-du "$(head -1 "$OUT/U3/tideskv/rep1/du-after-age")" "epoch_s,bytes,wal_archive_bytes"
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
      sorted(t['du_after_age']), t['du_after_load']['bytes'] >= 0)")" "250 1000 0 None True ['bytes', 'wal_archive_bytes'] True"
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
EV=() START_LEVELS=()
OUT=$TMP/u6
uc_dispatch U6 2>/dev/null
check u6-cells "$(evs)" "tideskv:start tideskv:load tideskv:c1 tideskv:c8 tideskv:c64 tideskv:c256 kvrocks:start kvrocks:load kvrocks:c1 kvrocks:c8 kvrocks:c64 kvrocks:c256"
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

# ---- shellcheck ----
if command -v shellcheck >/dev/null; then
  if (cd "$HERE" && shellcheck -x uc_cases_run.sh test_uc_cases_run.sh); then check shellcheck ok ok; else check shellcheck fail ok; fi
fi
exit $fail
