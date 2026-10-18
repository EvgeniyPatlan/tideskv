#!/usr/bin/env bash
# Tests for workloads.sh (registered in ctest as bench_workloads).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source-path=SCRIPTDIR source=workloads.sh
source "$HERE/workloads.sh"
fail=0
check() { if [[ $2 != "$3" ]]; then echo "FAIL $1: got [$2] want [$3]"; fail=1; else echo "ok   $1"; fi; }
calls=()
# shellcheck disable=SC2317  # invoked indirectly as the wl_* callback
record() { local IFS=$'\x1f'; calls+=("$*"); }
has() { [[ $1 == *"$2"* ]] && echo yes || echo no; }

wl_sizes quick
check quick-keys "$S_KEYS" 4000000
check quick-hashes "$H_COUNT x $H_FIELDS" "5 x 10000"
check quick-bh "$BH_COUNT x $BH_FIELDS" "1000 x 1000"
check quick-del-rate "$W6_DEL_RATE" 20
wl_sizes full
check full-keys "$S_KEYS" 180000000
check full-bh "$BH_COUNT x $BH_FIELDS" "30000 x 1000"
check full-del-rate "$W6_DEL_RATE" 100

wl_sizes quick
calls=(); wl_load strings record
check strings-calls "${#calls[@]}" 1
check strings-seq "$(has "${calls[0]}" "--key-pattern=P:P")" yes
check strings-all "$(has "${calls[0]}" "--requests=allkeys")" yes
check strings-prefix "$(has "${calls[0]}" "--key-prefix=s:")" yes
calls=(); wl_load hashes record
check hashes-calls "${#calls[@]}" 5
check hashes-cmd "$(has "${calls[4]}" "--command=HSET h:5 __key__ __data__")" yes
calls=(); wl_load zsets record
check zsets-cmd "$(has "${calls[0]}" "--command=ZADD z:1 __key__ __key__")" yes
check zsets-prefix "$(has "${calls[0]}" "--key-prefix=")" yes
calls=(); wl_load lists record
check lists-cmd "$(has "${calls[0]}" "--command=LPUSH l:1 __data__")" yes
calls=(); wl_load bighashes record
check bighashes-calls "${#calls[@]}" 1
check bighashes-prefix "$(has "${calls[0]}" "--key-prefix=bh:")" yes

calls=(); wl_run W1 record
check w1-ratio "$(has "${calls[0]}" "--ratio=1:1")" yes
check w1-random "$(has "${calls[0]}" "--key-pattern=R:R")" yes
calls=(); wl_run W2 record
check w2-ratio "$(has "${calls[0]}" "--ratio=1:10")" yes
calls=(); wl_run W3 record
check w3-hget "$(has "${calls[0]}" "--command=HGET h:5 __key__")" yes
check w3-count "$(grep -o -- '--command=' <<<"${calls[0]}" | wc -l)" 10
calls=(); wl_run W4 record
check w4-range "$(has "${calls[0]}" "--command=ZRANGEBYSCORE z:1 __key__ +inf LIMIT 0 10")" yes
calls=(); wl_run W5 record
check w5-lpop "$(has "${calls[0]}" "--command=LPOP l:1")" yes
calls=(); wl_run W6 record
check w6-main "$(has "${calls[0]}" "--ratio=1:1")" yes
calls=(); wl_run_w6_del record
check w6-del "$(has "${calls[0]}" "--command=DEL __key__")" yes
check w6-rate "$(has "${calls[0]}" "--rate-limiting=20")" yes
check w6-one-client "$(has "${calls[0]}" "--clients=1")" yes

for ds in hashes zsets lists bighashes; do
  calls=(); wl_load "$ds" record
  for c in "${calls[@]}"; do
    check "$ds-no-key-pattern" "$(has "$c" "--key-pattern")" no
    check "$ds-threads" "$(has "$c" "--threads=1")" yes
    check "$ds-clients" "$(has "$c" "--clients=1")" yes
  done
done
calls=(); wl_load strings record
check strings-threads "$(has "${calls[0]}" "--threads=4")" yes

for w in $WORKLOADS; do
  check "probe-$w-nonempty" "$( [[ -n $(wl_probe "$w") ]] && echo yes || echo no)" yes
done
check probe-w3 "$(wl_probe W3 | grep -c '^HGET h:1 ')" 1
check verify-lines "$(wl_verify_cmds | wc -l)" 5
check verify-hlen "$(wl_verify_cmds | grep -c '^9900 HLEN h:1$')" 1
check logical "$( (( $(wl_logical_bytes) > 400000000 )) && echo yes || echo no)" yes

if command -v shellcheck >/dev/null; then
  shellcheck -x "$HERE/workloads.sh" "$HERE/test_workloads.sh" "$HERE/lib.sh" "$HERE/run.sh" && echo "ok   shellcheck" || fail=1
else
  echo "skip shellcheck (not installed)"
fi
bash -n "$HERE/workloads.sh" || fail=1
bash -n "$HERE/lib.sh" "$HERE/run.sh" || fail=1
exit $fail
