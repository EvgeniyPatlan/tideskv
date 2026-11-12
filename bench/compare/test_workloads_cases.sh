#!/usr/bin/env bash
# Tests for workloads_cases.sh (registered in ctest as bench_workloads_cases).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source-path=SCRIPTDIR source=workloads.sh
source "$HERE/workloads.sh"
# shellcheck source-path=SCRIPTDIR source=workloads_cases.sh
source "$HERE/workloads_cases.sh"
fail=0
check() { if [[ $2 != "$3" ]]; then echo "FAIL $1: got [$2] want [$3]"; fail=1; else echo "ok   $1"; fi; }
calls=()
# shellcheck disable=SC2317  # invoked indirectly as the wc_* callback
record() { local IFS=$'\x1f'; calls+=("$*"); }
has() { [[ $1 == *"$2"* ]] && echo yes || echo no; }
# number of memtier words in a recorded call that equal $2
count_word() { local w n=0; local IFS=$'\x1f'; for w in $1; do [[ $w == "$2" ]] && n=$((n + 1)); done; echo "$n"; }

check cases "$CASES" "C1 C2 C3 C4 C5 C6 C7 C8"
check datasets "$CASE_DATASETS" "cache cache_exp sessions counters sets boards feeds batch"
check rate "$CASE_RATE" 10000

wc_sizes quick
check q-c1 "$C1_KEYS $C1_VALUE $C1_EXPIRE_S" "200000 2048 30"
check q-c2 "$C2_KEYS $C2_FIELDS $C2_VALUE" "100000 30 50"
check q-c3 "$C3_KEYS" 100000
check q-c4 "$C4_SETS x $C4_MEMBERS" "10 x 20000"
check q-c5 "$C5_BOARDS x $C5_MEMBERS" "5 x 100000"
check q-c6 "$C6_LISTS x $C6_AVG" "20000 x 200"
check q-c7 "$C7_KEYS" 1000000
check q-repeats "$CASE_REPEATS" 1
wc_sizes medium
check m-c1 "$C1_KEYS $C1_VALUE $C1_EXPIRE_S" "3000000 2048 120"
check m-c2 "$C2_KEYS $C2_FIELDS $C2_VALUE" "2000000 30 50"
check m-c3 "$C3_KEYS" 1000000
check m-c4 "$C4_SETS x $C4_MEMBERS" "100 x 100000"
check m-c5 "$C5_BOARDS x $C5_MEMBERS" "20 x 1000000"
check m-c6 "$C6_LISTS x $C6_AVG" "20000 x 500"
check m-c7 "$C7_KEYS" 10000000
check m-repeats "$CASE_REPEATS" 3
rc=0; wc_sizes huge 2>/dev/null || rc=$?
check bad-size "$rc" 2

GIB=$((1024 * 1024 * 1024))
b=$(wc_logical_bytes)
check m-logical "$( ((b > 10 * GIB && b < 15 * GIB)) && echo yes || echo no)" yes

wc_sizes quick
calls=(); wc_load cache record
check cache-calls "${#calls[@]}" 1
check cache-range "$(has "${calls[0]}" "--key-maximum=160000")" yes
check cache-prefix "$(has "${calls[0]}" "--key-prefix=c:")" yes
check cache-no-ttl "$(has "${calls[0]}" " EX ")" no
calls=(); wc_load cache_exp record
check exp-ex "$(has "${calls[0]}" "SET __key__ __data__ EX 30")" yes
check exp-min "$(has "${calls[0]}" "--key-minimum=160001")" yes
check exp-max "$(has "${calls[0]}" "--key-maximum=200000")" yes
calls=(); wc_load sessions record
check sess-prefix "$(has "${calls[0]}" "--key-prefix=sess:")" yes
check sess-fields "$(grep -o ' f[0-9]* __data__' <<<"${calls[0]}" | wc -l)" 30
check sess-f30 "$(has "${calls[0]}" " f30 __data__")" yes
calls=(); wc_load counters record
check ctr "$(has "${calls[0]}" "--command=SET __key__ 0")" yes
check ctr-prefix "$(has "${calls[0]}" "--key-prefix=ctr:")" yes
calls=(); wc_load sets record
check sets-calls "${#calls[@]}" "$C4_SETS"
check sets-cmd "$(has "${calls[9]}" "--command=SADD set:10 __key__")" yes
check sets-prefix "$(has "${calls[0]}" "--key-prefix=m:")" yes
calls=(); wc_load boards record
check boards-calls "${#calls[@]}" "$C5_BOARDS"
check boards-cmd "$(has "${calls[0]}" "--command=ZADD lb:1 __key__ __key__")" yes
check boards-prefix "$(has "${calls[0]}" $'--key-prefix=\x1f')" yes
calls=(); wc_load feeds record
check feeds-cmd "$(has "${calls[0]}" "--command=LPUSH __key__ __data__")" yes
check feeds-prefix "$(has "${calls[0]}" "--key-prefix=feed:")" yes
check feeds-random "$(has "${calls[0]}" "--command-key-pattern=R")" yes
check feeds-requests "$(has "${calls[0]}" "--requests=4000000")" yes
check feeds-1x1 "$(has "${calls[0]}" "--threads=1")$(has "${calls[0]}" "--clients=1")" yesyes
calls=(); wc_load batch record
check batch-prefix "$(has "${calls[0]}" "--key-prefix=mk:")" yes
check batch-size "$(has "${calls[0]}" "--data-size=100")" yes
check batch-seq "$(has "${calls[0]}" "--key-pattern=P:P")" yes
for ds in $CASE_DATASETS; do
  calls=(); wc_load "$ds" record
  for c in "${calls[@]}"; do
    check "$ds-threads" "$(has "$c" "--threads=")$(has "$c" "--clients=")" yesyes
    [[ $c == *--data-size* ]] && check "$ds-random-data" "$(has "$c" "--random-data")" yes
  done
done
rc=0; wc_load nope record 2>/dev/null || rc=$?
check load-unknown "$rc" 2

calls=(); wc_run C1 record
ratios=0
while read -r n; do ratios=$((ratios + n)); done < <(grep -o -- '--command-ratio=[0-9]*' <<<"${calls[0]//$'\x1f'/$'\n'}" | cut -d= -f2)
check c1-ratio-sum "$ratios" 20
check c1-get "$(has "${calls[0]}" "--command=GET __key__")" yes
check c1-ex60 "$(has "${calls[0]}" "EX 60")" yes
check c1-ex3600 "$(has "${calls[0]}" "EX 3600")" yes
calls=(); wc_run C2 record
check c2-hgetall "$(has "${calls[0]}" "--command=HGETALL __key__")" yes
check c2-prefix "$(has "${calls[0]}" "--key-prefix=sess:")" yes
calls=(); wc_run C3 record
check c3-commands "$(count_word "${calls[0]}" "--command-key-pattern=G")" 12
# memtier 2.1.4 refuses --key-stddev/--key-median with --command: sigma 1000 = a +-3 sigma range around 50000
check c3-no-stddev "$(has "${calls[0]}" "--key-stddev")" no
check c3-range "$(has "${calls[0]}" "--key-minimum=47000")$(has "${calls[0]}" "--key-maximum=53000")" yesyes
check c3-hincrby "$(has "${calls[0]}" "--command=HINCRBY hstats:10 __key__ 1")" yes
calls=(); wc_run C4 record
check c4-triples "$(grep -o -- '--command=' <<<"${calls[0]}" | wc -l)" 30
check c4-sscan "$(has "${calls[0]}" "--command=SSCAN set:10 0")" yes
check c4-range "$(has "${calls[0]}" "--key-maximum=40000")" yes
calls=(); wc_run C5 record
check c5-triples "$(grep -o -- '--command=' <<<"${calls[0]}" | wc -l)" 20
check c5-zrevrank "$(has "${calls[0]}" "--command=ZREVRANK lb:5 __key__")" yes
check c5-range "$(has "${calls[0]}" "--key-maximum=200000")" yes
calls=(); wc_run C6 record
check c6-ltrim "$(has "${calls[0]}" "--command=LTRIM __key__ 0")" yes
check c6-range "$(has "${calls[0]}" "--key-maximum=20000")" yes
calls=(); wc_run C7 record
mget=$(grep '^--command=MGET ' <<<"${calls[0]//$'\x1f'/$'\n'}")
check c7-mget-keys "$(grep -o '__key__' <<<"$mget" | wc -l)" 20
mset=$(grep '^--command=MSET ' <<<"${calls[0]//$'\x1f'/$'\n'}")
check c7-mset-keys "$(grep -o '__key__' <<<"$mset" | wc -l)" 10
check c7-mset-data "$(grep -o '__data__' <<<"$mset" | wc -l)" 10
check c7-ratios "$(has "${calls[0]}" "--command-ratio=9")$(has "${calls[0]}" "--command-ratio=1")" yesyes
rc=0; wc_run C8 record 2>/dev/null || rc=$?
check c8-run-rc "$rc" 2
rc=0; wc_run C9 record 2>/dev/null || rc=$?
check c9-run-rc "$rc" 2

calls=(); wc_c8_parts record
check c8-calls "${#calls[@]}" 4
check c8-names "$(for c in "${calls[@]}"; do printf '%s ' "${c%%$'\x1f'*}"; done)" "c1 c2 c5 scan "
check c8-c1-threads "$(has "${calls[0]}" "--threads=2")$(has "${calls[0]}" "--clients=16")" yesyes
check c8-c5-threads "$(has "${calls[2]}" "--threads=2")$(has "${calls[2]}" "--clients=16")" yesyes
check c8-scan "$(has "${calls[3]}" "--command=SCAN 0 MATCH c:1* COUNT 1000")" yes
check c8-scan-conns "$(has "${calls[3]}" "--threads=1")$(has "${calls[3]}" "--clients=5")$(has "${calls[3]}" "--rate-limiting=1")" yesyesyes

check rate-4x50 "$(wc_rate_per_conn 4 50)" 50
check rate-3x7 "$(wc_rate_per_conn 3 7)" 476
check rate-200x100 "$(wc_rate_per_conn 200 100)" 1

for c in C1 C2 C3 C4 C5 C6 C7; do
  check "probe-$c" "$(wc_probe "$c" | wc -l)" 2
done
rc=0; wc_probe C9 2>/dev/null >/dev/null || rc=$?
check probe-unknown "$rc" 2

check verify-lines "$(wc_verify_cmds | wc -l)" 5
check verify-hlen "$(wc_verify_cmds | grep -c '^30 HLEN sess:1$')" 1
check verify-scard "$(wc_verify_cmds | grep -c '^19800 SCARD set:1$')" 1
check verify-zcard "$(wc_verify_cmds | grep -c '^99000 ZCARD lb:1$')" 1
check verify-llen "$(wc_verify_cmds | grep -c '^1 LLEN feed:1$')" 1
check verify-dbsize "$(wc_verify_cmds | grep -c ' DBSIZE$')" 1

bash -n "$HERE/workloads_cases.sh" || fail=1
exit $fail
