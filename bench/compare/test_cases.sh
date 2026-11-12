#!/usr/bin/env bash
# Tests for cases.sh, the run.sh --cases loop (registered in ctest as bench_cases). docker, cli, memtier, the clock and
# the server lifecycle are shell-function fakes; nothing here starts a container.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail=0
check() { if [[ $2 != "$3" ]]; then echo "FAIL $1: got [$2] want [$3]"; fail=1; else echo "ok   $1"; fi; }
yn() { if "$@"; then echo yes; else echo no; fi; }

# ---- option rules (run.sh exits before bench_init, so these run nothing) ----
# sources run.sh with the given options in a fresh bash and prints the named variables
# shellcheck disable=SC2016  # the script is bash -c's
opts() {
  bash -c 'f=$1 v=$2 o=""; shift 2; source "$f" "$@" || exit; for n in $v; do o+="${!n} "; done; echo "${o% }"' \
    _ "$HERE/run.sh" "$@"
}
check opt-cases-medium "$(opts SIZE --cases)" medium
check opt-cases-quick "$(opts SIZE --cases --quick)" quick
check opt-cases-explicit-medium "$(opts SIZE --cases --medium)" medium
check opt-cases-large "$(bash "$HERE/run.sh" --cases --large 2>&1; echo $?)" "--cases cannot be combined with --large
2"
check opt-plain-full "$(opts 'SIZE CASES_MODE')" "full 0"
check opt-plain-quick "$(opts 'SIZE CASES_MODE' --quick)" "quick 0"

# shellcheck source-path=SCRIPTDIR source=run.sh
source "$HERE/run.sh"
set +e  # the functions under test return non-zero on purpose; each check reads its own result

# ---- fakes ----
OUT=$T/out TS=test NET=net TIDESKV_IMAGE=tideskv-bench:test THREADS=4 CLIENTS=50 WARMUP=1 DURATION=2
FAKE_GO=pass
log() { echo "log: $*" >>"$T/log"; }
# shellcheck disable=SC2317  # fakes are called indirectly
go() { :; }
# shellcheck disable=SC2317
docker() {
  local a
  echo "docker $*" >>"$T/docker.calls"
  if [[ $1 == run ]]; then
    for a in "$@"; do [[ $a == "$GO_IMAGE" ]] && { fake_go; return; }; done
  fi
  if [[ $1 == inspect ]]; then echo true; fi
  return 0
}
# shellcheck disable=SC2317
fake_go() {
  local c s act
  if [[ $FAKE_GO == fail ]]; then echo "go: github.com/redis/go-redis/v9@v9.17.2: module lookup disabled by GOPROXY=off"; return 1; fi
  for c in $CASES; do
    act=pass
    for s in 1 2; do
      if [[ $FAKE_GO == c3 && $c == C3 && $s == 2 ]]; then
        printf '{"Action":"output","Test":"TestKvrocksCases/C3/seed-2","Output":"step 7 diverged\\n"}\n'
        printf '{"Action":"fail","Test":"TestKvrocksCases/C3/seed-2"}\n'; act=fail
      else
        printf '{"Action":"pass","Test":"TestKvrocksCases/%s/seed-%s"}\n' "$c" "$s"
      fi
    done
    printf '{"Action":"%s","Test":"TestKvrocksCases/%s"}\n' "$act" "$c"
  done
  [[ $FAKE_GO == pass ]]
}
# shellcheck disable=SC2317
memtier() {
  mkdir -p "$(dirname "$OUT/$1")"
  echo '{}' >"$OUT/$1.json"
  echo "$*" >>"$T/memtier.calls"
  if [[ $1 == */load/cache_exp-* && -n ${FAKE_LOAD_SECS:-} ]]; then echo $(( $(_q_now) + FAKE_LOAD_SECS )) >"$T/clock"; fi
  return 0
}
# shellcheck disable=SC2317
cli() {
  local s
  echo "$*" >>"$T/cli.calls"
  s=$(cat "$T/scans")
  case "$*" in
    PING) echo PONG ;;
    "DBSIZE SCAN") echo $((s + 1)) >"$T/scans"; echo OK ;;
    DBSIZE) if [[ $SERVER_NAME == kvrocks ]] && ((s == 0)); then echo 0; else echo $((100000000 + s)); fi ;;
    "INFO keyspace") echo "# Keyspace" ;;
    HLEN*|SCARD*|ZCARD*|LLEN*) echo 99999999 ;;
    *) echo OK ;;
  esac
}
# shellcheck disable=SC2317
_q_now() { cat "$T/clock"; }
# shellcheck disable=SC2317
_q_sleep() { echo $(( $(cat "$T/clock") + 1 )) >"$T/clock"; }
# shellcheck disable=SC2317
settle() { echo 0 >"$1"; }
# shellcheck disable=SC2317
cg_sample() { echo "1 1"; }
# shellcheck disable=SC2317
_du() { echo 4096; }
# shellcheck disable=SC2317
start_server() { SERVER_NAME=$1 SERVER_CONTAINER=tideskv-bench-$1-$TS SERVER_PORT=6379; mkdir -p "$3" "$OUT/$1"; }
# shellcheck disable=SC2317
stop_server() { :; }
# shellcheck disable=SC2317
parity_check() { :; }
# shellcheck disable=SC2317
remove_data() { :; }

reset() {
  rm -rf "$OUT" "$T"/*.calls "$T/log"; mkdir -p "$OUT"
  : >"$T/memtier.calls"; echo 0 >"$T/scans"; echo 0 >"$T/clock"
}
diff_status() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get(sys.argv[2]))' "$OUT/cases-diff.json" "$1"; }
expiry_field() { python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' "$OUT/$CUR/expiry.json" "$1"; }

# ---- diverged-skips ----
reset; wc_sizes quick; FAKE_GO=c3
run_case_diff
check diverged-c3 "$(diff_status C3)" diverged
check diverged-c1-pass "$(diff_status C1)" pass
check diverged-c8-pass "$(diff_status C8)" pass
check diverged-log "$(yn grep -q 'step 7 diverged' "$OUT/cases-diff.log")" yes
check diverged-passed-list "$(cases_passed)" "C1 C2 C4 C5 C6 C7 C8"
run_cases_server tideskv
check diverged-no-c3-memtier "$(grep -c '/C3\.' "$T/memtier.calls")" 0
check diverged-c1-timed "$(yn test -f "$OUT/tideskv/relaxed/C1.r1.json")" yes
check diverged-diff-names "$(grep -c "run .*--name tideskv-bench-diff-tideskv-$TS " "$T/docker.calls")" 1
check diverged-diff-names-kv "$(grep -c "run .*--name tideskv-bench-diff-kvrocks-$TS " "$T/docker.calls")" 1
check diverged-go-env "$(yn grep -q "TIDESKV_OURS_ADDR=tideskv-bench-diff-tideskv-$TS:6379 .*TIDESKV_KVROCKS_ADDR=tideskv-bench-diff-kvrocks-$TS:6666 .*TIDESKV_DIFF_REQUIRED=1" "$T/docker.calls")" yes
check diverged-go-cmd "$(yn grep -q "$GO_IMAGE go test ./diff -run TestKvrocksCases -json" "$T/docker.calls")" yes
check diverged-go-src-ro "$(yn grep -q -- "-v $ROOT/tests/compat:/src:ro" "$T/docker.calls")" yes

# ---- diff-failed ----
reset; FAKE_GO=fail
run_case_diff
for c in $CASES; do check "diff-failed-$c" "$(diff_status "$c")" "diff did not run"; done
check diff-failed-reason "$(yn grep -q 'module lookup disabled' "$OUT/cases-diff.log")" yes
check diff-failed-passed "$(cases_passed)" ""
run_cases_server tideskv
check diff-failed-no-memtier "$(wc -l <"$T/memtier.calls")" 0
FAKE_GO=pass

# ---- kvrocks-dbsize-scan ----
reset; wc_sizes quick
CUR=kvrocks; start_server kvrocks relaxed "$OUT/data/kvrocks"; CASE_EXP_END=0
cases_expiry_check "$OUT/data/kvrocks"
n=$(expiry_field dbsize_after)
check kvrocks-dbsize-nonzero "$(yn test "$n" -gt 0)" yes
check kvrocks-dbsize-scan-ran "$(grep -c '^DBSIZE SCAN$' "$T/cli.calls")" 1
check kvrocks-keys-loaded "$(expiry_field keys_loaded)" "$C1_KEYS"
check kvrocks-keys-live "$(expiry_field keys_live_expected)" $((C1_KEYS * 8 / 10))
check kvrocks-bytes "$(expiry_field bytes_before) $(expiry_field bytes_after)" "4096 4096"

# ---- expiry-wait-from-own-load ----
reset; CUR=tideskv; start_server tideskv relaxed "$OUT/data/tideskv"
C1_EXPIRE_S=30 CASE_EXP_END=100; echo 100 >"$T/clock"
cases_expiry_check "$OUT/data/tideskv"
check expiry-wait-30 "$(expiry_field waited_s)" 30
echo 140 >"$T/clock"
cases_expiry_check "$OUT/data/tideskv"
check expiry-wait-0 "$(expiry_field waited_s)" 0
# the countdown starts at this server's own cache_exp load end: a load that ends 50 s later moves it by 50 s
reset; wc_sizes quick; CUR=tideskv; start_server tideskv relaxed "$OUT/data/tideskv"
echo 10 >"$T/clock"; FAKE_LOAD_SECS=50
cases_load_all
check expiry-load-end "$CASE_EXP_END" 60
check expiry-load-first "$(head -1 "$T/memtier.calls" | cut -d' ' -f1)" tideskv/load/cache_exp-1
FAKE_LOAD_SECS=""

# ---- repeats ----
for size in quick medium; do
  reset; wc_sizes "$size"; FAKE_GO=pass
  run_case_diff
  run_cases_server kvrocks
  for level in relaxed strict; do
    for c in $CASES; do
      part=""; [[ $c == C8 ]] && part=.c1
      got=$(cd "$OUT/kvrocks/$level" && find . -maxdepth 1 -name "$c.r[0-9]*$part.json" ! -name '*warmup*' | sort | xargs)
      want=./$c.r1$part.json; [[ $size == medium ]] && want="./$c.r1$part.json ./$c.r2$part.json ./$c.r3$part.json"
      check "repeats-$size-$level-$c" "$got" "$want"
      check "repeats-$size-$level-$c-du" "$(yn test -f "$OUT/kvrocks/$level/$c.du")" yes
    done
  done
  check "repeats-$size-disk" "$(yn test -f "$OUT/kvrocks/disk.json")" yes
  check "repeats-$size-expiry" "$(yn test -f "$OUT/kvrocks/expiry.json")" yes
done

# ---- rate-pass (and the flags every timed run carries) ----
reset; wc_sizes quick
run_case_diff
run_cases_server tideskv
for c in C1 C2 C3 C4 C5 C6 C7; do
  check "rate-$c" "$(yn test -f "$OUT/tideskv/strict/$c.rate.json" -a -f "$OUT/tideskv/strict/$c.rate.status")" yes
done
check rate-c8-none "$(find "$OUT/tideskv" -name 'C8.rate*' | wc -l)" 0
check rate-limit "$(grep '/C1\.rate ' "$T/memtier.calls" | grep -c -- '--rate-limiting=50')" 2
check rate-status "$(cat "$OUT/tideskv/relaxed/C1.rate.status")" ok
for p in c1 c2 c5 scan; do check "c8-part-$p" "$(yn test -f "$OUT/tideskv/relaxed/C8.r1.$p.json")" yes; done
check c8-part-flags "$(grep '/C8\.r1\.c1 ' "$T/memtier.calls" | grep -- '--threads=2' | grep -c -- '--distinct-client-seed --randomize')" 2
check c8-part-no-override "$(grep '/C8\.r1\.' "$T/memtier.calls" | grep -c -- '--threads=4')" 0
check c8-status "$(cat "$OUT/tideskv/relaxed/C8.r1.status")" ok
check timed-flags "$(grep '/relaxed/C1\.r1 ' "$T/memtier.calls" | grep -- '--threads=4 --clients=50 --pipeline=1' | grep -c -- '--randomize')" 1
check timed-files "$(for e in status cpu stats.jsonl settle; do [[ -f $OUT/tideskv/relaxed/C1.r1.$e ]] && echo "$e"; done | wc -l)" 4
check timed-no-info "$(find "$OUT/tideskv" -name '*.info0' | wc -l)" 0
check disk-json "$(python3 -c 'import json,sys; print(sorted(json.load(open(sys.argv[1]))))' "$OUT/tideskv/disk.json")" \
  "['after_load_bytes', 'end_bytes', 'load_seconds', 'logical_bytes']"

# ---- the Go diff container is tracked, so bench_cleanup removes it ----
reset; CONTAINERS=(); FAKE_GO=pass
run_case_diff
check diff-go-tracked "$(printf '%s\n' "${CONTAINERS[@]}" | grep -cx "tideskv-bench-diff-go-$TS")" 1
check diff-go-named "$(grep -c -- "--name tideskv-bench-diff-go-$TS " "$T/docker.calls")" 1

# ---- smoke: every case must pass the diff ----
reset; SIZE=quick; wc_sizes quick; FAKE_GO=c3; rc=0
run_cases_smoke || rc=$?
check smoke-diverged-fails "$(yn test "$rc" -ne 0)" yes
check smoke-diverged-no-ok "$(grep -c 'smoke ok' "$T/log")" 0
check smoke-diverged-names-c3 "$(yn grep -q 'C3=diverged' "$T/log")" yes
reset; FAKE_GO=fail; rc=0
run_cases_smoke || rc=$?
check smoke-not-run-fails "$(yn test "$rc" -ne 0)" yes
reset; FAKE_GO=pass; rc=0
run_cases_smoke || rc=$?
check smoke-pass "$rc $(grep -c 'smoke ok' "$T/log")" "0 1"

# ---- the shared load check: default wl_verify_cmds, --cases passes wc_verify_cmds ----
reset; SERVER_NAME=tideskv
# shellcheck disable=SC2317  # the fake verify list is called by load_check
check load-check-default "$( wl_verify_cmds() { echo "1 HLEN wl-default"; }; load_check; grep -c 'wl-default' "$T/cli.calls")" 1
reset; SERVER_NAME=tideskv
# shellcheck disable=SC2317  # the fake verify list is called by load_check
check load-check-cases "$( wc_verify_cmds() { echo "1 HLEN wc-cases"; }; load_check wc_verify_cmds; grep -c 'wc-cases' "$T/cli.calls")" 1
check cases-load-uses-shared "$(declare -f cases_load_all | grep -c 'load_check wc_verify_cmds')" 1
check no-copied-check "$(declare -F _cases_load_check >/dev/null && echo yes || echo no)" no

# ---- probe failure stops the case, not the server ----
reset; wc_sizes quick
run_case_diff
# shellcheck disable=SC2317
cli() { if [[ $1 == ZINCRBY ]]; then echo "WRONGTYPE Operation"; else echo OK; fi; }
CUR=tideskv; start_server tideskv relaxed "$OUT/data/tideskv"
run_case relaxed C5 1
check probe-status "$(cut -c1-17 "$OUT/tideskv/relaxed/C5.r1.status")" "probe: [ZINCRBY l"
check probe-no-memtier "$(grep -c '/C5\.' "$T/memtier.calls")" 0

if ((fail)); then echo "test_cases: FAILED"; exit 1; fi
echo "test_cases: all passed"
