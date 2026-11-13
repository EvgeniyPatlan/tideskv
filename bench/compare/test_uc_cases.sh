#!/usr/bin/env bash
# Tests for uc_cases.sh (registered in ctest as bench_uc_cases).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source-path=SCRIPTDIR source=uc_cases.sh
source "$HERE/uc_cases.sh"
# uc_conf_lines reads KVROCKS_BLOB_CONF from lib.sh (variables only, no side effects)
# shellcheck source-path=SCRIPTDIR source=lib.sh
source "$HERE/lib.sh"
fail=0
check() { if [[ $2 != "$3" ]]; then echo "FAIL $1: got [$2] want [$3]"; fail=1; else echo "ok   $1"; fi; }
calls=()
# shellcheck disable=SC2317  # invoked indirectly as the uc_* callback
record() { local IFS=$'\x1f'; calls+=("$*"); }
has() { [[ $1 == *"$2"* ]] && echo yes || echo no; }

uc_sizes full
check full-reps "$UC_REPS" 3
check u1-keys-full "$U1_KEYS" 20000000
# U7 points: N acked writes = 10/50/90% of the 256 MiB memtable at 2048 B values + 64 B key and entry overhead
check u7-times-full "$UC_U7_TIMES $UC_U7_ACKS" "5 12710 63550 114390"
check u7-entry "$UC_U7_VALUE $UC_U7_ENTRY $UC_U7_MEMTABLE" "2048 2112 268435456"
check u7-ack-timeout-full "$UC_U7_ACK_TIMEOUT" 900
check cases-order "$(uc_cases)" "U1 U2 U3 U4 U5 U6 U7 U8 F1"
check servers-u2 "$(uc_servers U2)" "tideskv kvrocks kvrocks-cron"
check servers-u5 "$(uc_servers U5)" "tideskv kvrocks kvrocks-blob"
check servers-f1 "$(uc_servers F1)" "tideskv kvrocks"
check servers-u1 "$(uc_servers U1)" "tideskv kvrocks"
check level-u6 "$(uc_level U6)" strict
check level-u7 "$(uc_level U7)" strict
check level-u1 "$(uc_level U1)" relaxed
# lib.sh writes and checks TidesKV's relaxed sync-mode from RELAXED: the suite's relaxed level is sync-mode none
check relaxed-sync-mode "$(BLOCK_CACHE_MB=2048 SERVER_WORKERS=4 _parity_expect tideskv relaxed | grep '^sync-mode')" "sync-mode none"
# F1-crit: logical x 1.4 (measured on-disk ratio) + 16 GiB WAL archive + logical / 2 (Kvrocks COMPACT) + 4 GiB slack
G=$((1024 * 1024 * 1024))
check disk-u1 "$(uc_disk_need U1)" $((20000000 * 1036 * 14 / 10 + 16 * G + 20000000 * 1036 / 2 + 4 * G))
check disk-u7-writer "$(_uc_logical U7)" $((2000000 * 112 + 5 * 2 * (12710 + 63550 + 114390) * 2112))
check disk-u8-none "$(uc_disk_need U8)" $((20 * G))
check disk-u5 "$(uc_disk_need U5)" $((16000000 * 1036 * 14 / 10 + 16 * G + 16000000 * 1036 / 2 + 4 * G))
# spec section 3: every case's full-size need fits in 60 GiB (U1, the largest, is about 56.7 GiB)
for c in $(uc_cases); do
  check "disk-$c-under-60gib" "$(( $(uc_disk_need "$c") <= 60 * G ))" 1
done
check debt-fields "$([[ -n ${UC_DEBT_FIELDS_tideskv[*]} && -n ${UC_DEBT_FIELDS_kvrocks[*]} ]] && echo yes)" yes
check debt-kvrocks-pending "$(has "${UC_DEBT_FIELDS_kvrocks[*]}" estimate_pending_compaction_bytes)" yes
check phases-u1 "$(uc_phases U1)" "closed-gauss closed-uniform open25 open50 open75"
check phases-u2 "$(uc_phases U2)" "churn idle"
check phases-u3 "$(uc_phases U3)" "age closed open25 open50 open75"
check phases-u4 "$(uc_phases U4)" "mix"
check phases-u5 "$(uc_phases U5)" "v1k v4k v16k"
check phases-u6 "$(uc_phases U6)" "c1 c8 c64 c256"
check phases-u7-full "$(uc_phases U7)" "term-12710 kill-12710 term-63550 kill-63550 term-114390 kill-114390"
check phases-u8 "$(uc_phases U8)" "report-only"
check phases-f1 "$(uc_phases F1)" "matrix"

# extra server config lines (U2 cron, U5 blob, cache accounting)
check conf-tideskv-lines "$(uc_conf_lines tideskv | wc -l)" 0
check conf-kvrocks-lines "$(uc_conf_lines kvrocks | wc -l) ${#UC_CACHE_LINES[@]}" "0 0"
check conf-cron-lines "$(uc_conf_lines kvrocks-cron)" "compaction-checker-cron * * * * *"
check conf-blob-lines "$(uc_conf_lines kvrocks-blob)" "$(printf '%s\n' "${KVROCKS_BLOB_CONF[@]}")"
check conf-cache-accounting "$UC_CACHE_ACCOUNTING" charged
check conf-unknown "$(uc_conf_lines bogus 2>/dev/null || echo rc=$?)" "rc=2"

UC_RATE=37
calls=(); uc_phase U1 closed-gauss record
check u1-gauss-pattern "$(has "${calls[0]}" "--key-pattern=G:G")" yes
check u1-gauss-stddev "$(has "${calls[0]}" "--key-stddev=1000000")" yes
check u1-gauss-median "$(has "${calls[0]}" "--key-median=10000000")" yes
check u1-flags "$(has "${calls[0]}" "--distinct-client-seed")" yes
calls=(); uc_phase U1 closed-uniform record
check u1-uniform "$(has "${calls[0]}" "--key-pattern=R:R")" yes
calls=(); uc_phase U1 open50 record
check u1-open-rate "$(has "${calls[0]}" "--rate-limiting=37")" yes
check u1-open-ratio "$(has "${calls[0]}" "--ratio=1:10")" yes
check u1-open-gauss "$(has "${calls[0]}" "--key-pattern=G:G")" yes
calls=(); uc_load U1 record
check u1-load "$(has "${calls[0]}" "--key-prefix=u1:") $(has "${calls[0]}" "--data-size=1024") $(has "${calls[0]}" "--requests=allkeys")" "yes yes yes"

calls=(); uc_phase U2 churn record
check u2-expiry "$(has "${calls[0]}" "--expiry-range=120-1200")" yes
check u2-rate "$(has "${calls[0]}" "--rate-limiting=200") $(has "${calls[0]}" "--threads=4") $(has "${calls[0]}" "--clients=25")" "yes yes yes"
check u2-ratio "$(has "${calls[0]}" "--ratio=1:1")" yes
check u2-durations "$UC_U2_WRITE_S $UC_U2_IDLE_S $UC_U2_DU_EVERY" "3600 1800 60"
calls=(); uc_phase U2 idle record
check u2-idle-none "${#calls[@]}" 0

calls=(); uc_load U3 record
check u3-load-calls "${#calls[@]}" 1
check u3-load-size "$(has "${calls[0]}" "--data-size=200")" yes
calls=(); uc_phase U3 age record
check u3-age "$(has "${calls[0]}" "--ratio=1:0") $(has "${calls[0]}" "--key-pattern=R:R") $(has "${calls[0]}" "--pipeline=32") $(has "${calls[0]}" "--requests=1250000")" "yes yes yes yes"
calls=(); uc_phase U3 closed record
check u3-closed "$(has "${calls[0]}" "--ratio=4:1")" yes
calls=(); uc_phase U3 open75 record
check u3-open "$(has "${calls[0]}" "--rate-limiting=37")" yes
check u3-durations "$UC_U3_CLOSED_S $UC_U3_OPEN_S" "3600 600"

calls=(); uc_phase U4 mix record
check u4-ratios "$(grep -o -- '--command=' <<<"${calls[0]}" | wc -l) $(has "${calls[0]}" "--command-ratio=10") $(has "${calls[0]}" "--command-ratio=6") $(has "${calls[0]}" "--command-ratio=3") $(has "${calls[0]}" "--command-ratio=1")" "4 yes yes yes yes"
check u4-hgetall "$(has "${calls[0]}" "--command=HGETALL __key__")" yes
check u4-pattern "$(has "${calls[0]}" "--command-key-pattern=R")" yes
calls=(); uc_load U4 record
check u4-load "$(has "${calls[0]}" "--key-prefix=u4:") $(has "${calls[0]}" "f20 __data__") $(has "${calls[0]}" "--data-size=50")" "yes yes yes"

calls=(); uc_load U5 record v16k
check u5-v16k-load "${#calls[@]} $(has "${calls[0]}" "--key-maximum=1000000") $(has "${calls[0]}" "--data-size=16384")" "1 yes yes"
calls=(); uc_phase U5 v4k record
check u5-v4k-run "$(has "${calls[0]}" "--ratio=1:4") $(has "${calls[0]}" "--key-maximum=4000000") $(has "${calls[0]}" "--data-size=4096")" "yes yes yes"

calls=(); uc_phase U6 c256 record
check u6-conns "$(has "${calls[0]}" "--threads=4") $(has "${calls[0]}" "--clients=64") $(has "${calls[0]}" "--pipeline=1")" "yes yes yes"
calls=(); uc_phase U6 c1 record
check u6-c1 "$(has "${calls[0]}" "--threads=1") $(has "${calls[0]}" "--clients=1") $(has "${calls[0]}" "--ratio=1:0")" "yes yes yes"
check u6-secs "$UC_U6_S" 60
calls=(); uc_load U6 record
check u6-load "$(has "${calls[0]}" "--key-maximum=1000000")" yes
calls=(); uc_load U7 record
check u7-load "$(has "${calls[0]}" "--key-maximum=2000000")" yes
calls=(); uc_phase U7 term-12710 record; uc_load U8 record; uc_phase U8 report-only record; uc_load F1 record; uc_phase F1 matrix record
check no-callback-cases "${#calls[@]}" 0
check unknown-case "$(uc_phases ZZ 2>/dev/null || echo rc=$?)" "rc=2"

uc_sizes quick
check quick-reps "$UC_REPS" 1
check u1-keys-quick "$U1_KEYS" 2000000
check quick-durations "$UC_WARMUP $UC_DURATION" "10 30"
check u2-quick "$U2_RATE_PER_CONN $U2_EXPIRY $UC_U2_WRITE_S $UC_U2_IDLE_S $UC_U2_DU_EVERY" "50 30-300 600 300 15"
check u3-quick "$U3_KEYS $UC_U3_CLOSED_S $UC_U3_OPEN_S" "1000000 300 60"
check u4-quick "$U4_KEYS" 250000
check u6-quick "$UC_U6_S" 15
check u7-quick "$UC_U7_TIMES $UC_U7_ACKS $U7_KEYS $UC_U7_ACK_TIMEOUT" "1 1271 6355 11439 200000 300"
check phases-u7-quick "$(uc_phases U7)" "term-1271 kill-1271 term-6355 kill-6355 term-11439 kill-11439"
calls=(); uc_phase U2 churn record
check u2-quick-args "$(has "${calls[0]}" "--expiry-range=30-300") $(has "${calls[0]}" "--rate-limiting=50")" "yes yes"
calls=(); uc_load U5 record v1k
check u5-quick-keys "$(has "${calls[0]}" "--key-maximum=524288")" yes
calls=(); uc_load U5 record
check u5-load-all "${#calls[@]}" 3
check unknown-mode "$(uc_sizes bogus 2>/dev/null || echo rc=$?)" "rc=2"

if command -v shellcheck >/dev/null; then
  shellcheck -x -P "$HERE" "$HERE/uc_cases.sh" "$HERE/test_uc_cases.sh" && echo "ok   shellcheck" || fail=1
else
  echo "skip shellcheck (not installed)"
fi
bash -n "$HERE/uc_cases.sh" || fail=1
exit $fail
