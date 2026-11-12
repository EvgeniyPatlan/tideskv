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
wl_sizes medium
check medium-keys "$S_KEYS" 60000000
check medium-bh "$BH_COUNT x $BH_FIELDS" "10000 x 1000"
check medium-del-rate "$W6_DEL_RATE" 50
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
check w4-incr "$(has "${calls[0]}" "--command=ZINCRBY z:1 1 __key__")" yes
check w4-range "$(has "${calls[0]}" "--command=ZRANGEBYSCORE z:1 __key__ +inf LIMIT 0 10")" yes
calls=(); wl_run W5 record
check w5-lpop "$(has "${calls[0]}" "--command=LPOP l:1")" yes
calls=(); wl_run W6 record
check w6-main "$(has "${calls[0]}" "--ratio=1:1")" yes
calls=(); wl_run_w6_del record
check w6-del "$(has "${calls[0]}" "--command=DEL __key__")" yes
check w6-del-randomize "$(has "${calls[0]}" "--randomize")" yes
# memtier rate-limits per connection: 20 DEL/s is 20 connections at 1/s each, so it holds up to 1 s DEL latency
check w6-del-rate-per-conn "$(has "${calls[0]}" $'--rate-limiting=1\x1f')" yes
check w6-del-clients "$(has "${calls[0]}" "--clients=20")" yes
check w6-del-distinct-seed "$(has "${calls[0]}" "--distinct-client-seed")" yes
check w6-del-clients-var "$W6_DEL_CLIENTS" 20

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

# memtier's __data__ is a fixed pattern unless --random-data is set; fixed values make rewrites no-ops
for ds in $DATASETS; do
  calls=(); wl_load "$ds" record
  for c in "${calls[@]}"; do
    [[ $c == *--data-size* ]] && check "load-$ds-random-data" "$(has "$c" "--random-data")" yes
  done
done
for w in $WORKLOADS; do
  calls=(); wl_run "$w" record
  [[ ${calls[0]} == *--data-size* ]] && check "run-$w-random-data" "$(has "${calls[0]}" "--random-data")" yes
done
check run-flags "${MEMTIER_RUN_FLAGS[*]}" "--distinct-client-seed --randomize"
check probe-w4 "$(wl_probe W4 | head -1)" "ZINCRBY z:1 1 2"
for w in $WORKLOADS; do
  check "probe-$w-nonempty" "$( [[ -n $(wl_probe "$w") ]] && echo yes || echo no)" yes
done
check probe-w3 "$(wl_probe W3 | grep -c '^HGET h:1 ')" 1
check verify-lines "$(wl_verify_cmds | wc -l)" 5
check verify-hlen "$(wl_verify_cmds | grep -c '^9900 HLEN h:1$')" 1
check logical "$( (( $(wl_logical_bytes) > 400000000 )) && echo yes || echo no)" yes

# W7 (--large): 256 KiB values, working set set by --key-maximum, 8 threads x 4 clients
wl_sizes large
check large-keys "$W7_KEYS" 131072
check large-datasets "$DATASETS" large
check large-logical "$(wl_logical_bytes)" $((131072 * (262144 + 8)))
check large-verify "$(wl_verify_cmds)" "129761 DBSIZE"
calls=(); wl_load large record
check large-load-calls "${#calls[@]}" 1
for a in --ratio=1:0 --data-size=262144 --random-data --key-prefix=L: --key-pattern=P:P --key-maximum=131072 \
         --requests=allkeys; do
  check "large-load$a" "$(has "${calls[0]}$'\x1f'" "$a"$'\x1f')" yes
done
check large-keys-r1 "$(wl_w7_keys R1)" 4096
check large-keys-r4 "$(wl_w7_keys R4)" 16384
check large-keys-r32 "$(wl_w7_keys R32)" 131072
check large-keys-sweep "$(wl_w7_keys R32w16)" 131072
for p in R1 R4 R32 M32 R32w16; do
  calls=(); wl_run_w7 "$p" record
  c="${calls[0]}"$'\x1f'
  for a in --data-size=262144 --random-data --distinct-client-seed --randomize --threads=8 --clients=4 --pipeline=1 \
           --key-prefix=L: --key-pattern=R:R --key-minimum=1 "--key-maximum=$(wl_w7_keys "$p")"; do
    check "w7-$p$a" "$(has "$c" "$a"$'\x1f')" yes
  done
done
calls=(); wl_run_w7 R4 record
check w7-r4-get-only "$(has "${calls[0]}" "--ratio=0:1")" yes
check w7-r4-keymax "$(has "${calls[0]}" "--key-maximum=16384")" yes
calls=(); wl_run_w7 M32 record
check w7-m32-ratio "$(has "${calls[0]}" "--ratio=1:9")" yes
check w7-m32-keymax "$(has "${calls[0]}"$'\x1f' $'--key-maximum=131072\x1f')" yes
rc=0; wl_run_w7 X9 record 2>/dev/null || rc=$?
check w7-bad-phase "$((rc != 0))" 1
rc=0; wl_w7_keys R64 2>/dev/null || rc=$?
check w7-bad-ratio "$((rc != 0))" 1
wl_sizes large-smoke
check large-smoke-keys "$W7_KEYS" 512
check large-smoke-r1 "$(wl_w7_keys R1)" 16
check large-smoke-r32 "$(wl_w7_keys R32)" 512
# going back to a W1-W6 size clears W7 and restores the datasets
wl_sizes quick
check quick-after-large "$W7_KEYS $DATASETS" "0 strings hashes zsets lists bighashes"
check quick-verify-after-large "$(wl_verify_cmds | wc -l)" 5


# ---- run.sh helpers, sourced without running (Docker is stubbed) ----
# shellcheck source=run.sh
source "$HERE/run.sh"
T=$(mktemp -d); trap 'chmod -R u+rwx "$T"; rm -rf "$T"' EXIT
OUT=$T/out; mkdir -p "$OUT"; TS="test"; SERVER_CONTAINER="none"

# I1: a failing memtier must fail the load and stop the loop
MT_CALLS=0 MT_FAIL_AT=2
# shellcheck disable=SC2317
memtier() { MT_CALLS=$((MT_CALLS + 1)); ((MT_CALLS != MT_FAIL_AT)); }
wl_sizes quick; CUR=tideskv LOAD_NAME=hashes LOAD_N=1
rc=0; wl_load hashes _load_cb || rc=$?
check load-fail-rc "$((rc != 0))" 1
check load-fail-stops "$MT_CALLS" 2
MT_CALLS=0 MT_FAIL_AT=99 LOAD_N=1 rc=0; wl_load lists _load_cb || rc=$?
check load-ok-rc "$rc" 0
check load-ok-count "$MT_CALLS $LOAD_N" "5 6"
MT_CALLS=0 MT_FAIL_AT=1 rc=0; wl_load strings _load_cb || rc=$?
check load-strings-fail-rc "$((rc != 0))" 1

# quiesce: busy samples, then a quiet 5 s window; the clock advances one second per sleep
Q_T=0
# shellcheck disable=SC2317
_q_now() { echo "$Q_T"; }
# shellcheck disable=SC2317
_q_sleep() { Q_T=$((Q_T + 1)); }
# cumulative cpu_usec and written bytes: 1 core busy and 10 MB/s for 8 s, then 1% CPU and no writes
# shellcheck disable=SC2317
cg_sample() {
  if ((Q_T < 8)); then echo "$((Q_T * 1000000)) $((Q_T * 10000000))"
  else echo "$((8000000 + (Q_T - 8) * 10000)) 80000000"; fi
}
QUIESCE_MAX=60
check quiesce-settles "$(quiesce)" 13
check settle-text "$(settle_text 13)" "settled after 13s"
check settle-text-capped "$(settle_text "600 capped")" "did not settle within 600s"
# never quiet: capped at QUIESCE_MAX
# shellcheck disable=SC2317
cg_sample() { echo "$((Q_T * 1000000)) 0"; }
Q_T=0 QUIESCE_MAX=20
check quiesce-capped "$(quiesce)" "20 capped"
# quiet CPU but steady writes is not quiet
# shellcheck disable=SC2317
cg_sample() { echo "0 $((Q_T * 4000000))"; }
Q_T=0
check quiesce-writes-capped "$(quiesce)" "20 capped"
# already idle: one 5 s window
# shellcheck disable=SC2317
cg_sample() { echo "0 0"; }
Q_T=0
check quiesce-idle "$(quiesce)" 5
# shellcheck disable=SC2317
cg_sample() { return 1; }
Q_T=0 rc=0; quiesce >/dev/null || rc=$?
check quiesce-sample-fail "$((rc != 0))" 1

# I5: --out is made absolute, and a non-empty data dir is refused
(cd "$T" && resolve_out rel/run) >"$T/ro" 2>/dev/null
check out-absolute "$(cat "$T/ro")" "$T/rel/run"
mkdir -p "$T/used/data/tideskv"
rc=0; resolve_out "$T/used" >/dev/null 2>&1 || rc=$?
check out-nonempty-refused "$((rc != 0))" 1
mkdir -p "$T/empty/data"
check out-empty-data-ok "$(resolve_out "$T/empty")" "$T/empty"

# I4: page-cache eviction runs dd per file on the host, or in a container when a file is unreadable
DOCKER_CALLS=()
# shellcheck disable=SC2317
docker() { DOCKER_CALLS+=("$*"); }
# shellcheck disable=SC2317
sync() { :; }
mkdir -p "$T/d1/sub"; echo a >"$T/d1/sub/f1"; echo b >"$T/d1/f2"
check evict-host-rc "$(evict_cache "$T/d1" && echo 0)" 0
check evict-host-no-docker "${#DOCKER_CALLS[@]}" 0
chmod 000 "$T/d1/f2"
evict_cache "$T/d1"
check evict-container "$(has "${DOCKER_CALLS[0]:-}" "tideskv-bench-evict-test")" yes
check evict-container-nocache "$(has "${DOCKER_CALLS[0]:-}" "iflag=nocache count=0")" yes
check evict-container-no-net "$(has "${DOCKER_CALLS[0]:-}" "--network none")" yes
chmod 644 "$T/d1/f2"

# I3: both servers get the same explicit open-file limit
check ulimit "$SERVER_NOFILE" 65536
# shellcheck disable=SC2016  # the literal text of lib.sh
check ulimit-in-start "$(grep -c -- '--ulimit "nofile=$SERVER_NOFILE:$SERVER_NOFILE"' "$HERE/lib.sh")" 1
# M4: stop_server ends with a host sync
check stop-sync "$(declare -f stop_server | grep -c '^ *sync;\?$')" 1

# M2: meta.json records the client side, the W6 target and the quiesce settings
TIDESKV_IMAGE=t WARMUP=10 DURATION=30 SIZE=quick DIRTY=0 RELAXED=none QUIESCE_MAX=60
wl_sizes quick
write_meta
meta() { python3 -c 'import json,sys; v=json.load(open(sys.argv[1]))[sys.argv[2]]; print(v)' "$OUT/meta.json" "$1"; }
check meta-threads "$(meta threads)" 4
check meta-clients "$(meta clients)" 50
check meta-client-cpus "$(meta client_cpus)" 8-15
check meta-flags "$(meta memtier_run_flags)" "--distinct-client-seed --randomize"
check meta-w6-target "$(meta w6_del_target_per_s)" 20
check meta-w6-clients "$(meta w6_del_clients)" 20
check meta-quiesce-max "$(meta quiesce_max_s)" 60
check meta-quiesce-window "$(meta quiesce_window_s)" 5
check meta-quiesce-cpu "$(meta quiesce_cpu_pct)" 5
check meta-quiesce-writes "$(meta quiesce_write_bytes)" 1048576
check meta-nofile "$(meta server_nofile)" 65536
check meta-cores "$( [[ -n $(meta server_cores) ]] && echo yes)" yes
unset -f docker sync memtier

# W7 (--large): config variants, parity, phase order, disk budget
RELAXED=none BLOCK_CACHE_MB=2048 SERVER_WORKERS=4
conf=$(write_server_conf tideskv relaxed)
check conf-tideskv-default "$(diff <(sed 's/SYNC_MODE/none/' "$HERE/tideskv.conf") "$conf" && echo same)" same
conf=$(write_server_conf kvrocks strict)
check conf-kvrocks-default "$(diff <(sed 's/SYNC_YESNO/yes/' "$HERE/kvrocks.conf") "$conf" && echo same)" same
check parity-default "$(_parity_expect kvrocks relaxed | head -2 | xargs)" "workers 4 rocksdb.block_cache_size 2048"
BLOCK_CACHE_MB=1024 SERVER_WORKERS=16
conf=$(write_server_conf tideskv relaxed)
check conf-tideskv-large "$(grep -E '^(workers|block-cache-size) ' "$conf" | xargs)" "workers 16 block-cache-size 1gb"
conf=$(write_server_conf kvrocks-blob relaxed)
check conf-blob-path "$conf" "$OUT/kvrocks-blob/kvrocks-relaxed.conf"
check conf-blob-cache "$(grep -c '^rocksdb.block_cache_size 1024$' "$conf")" 1
check conf-blob-workers "$(grep -c '^workers 16$' "$conf")" 1
check conf-blob-files "$(grep -c '^rocksdb.enable_blob_files yes$' "$conf")" 1
check conf-blob-min "$(grep -c '^rocksdb.min_blob_size 4096$' "$conf")" 1
conf=$(write_server_conf kvrocks relaxed)
check conf-kvrocks-no-blob "$(grep -c blob "$conf")" 0
check parity-tideskv-large "$(_parity_expect tideskv relaxed | head -2 | xargs)" "workers 16 block-cache-size 1073741824"
check parity-blob "$(_parity_expect kvrocks-blob relaxed | tail -2 | xargs)" \
  "rocksdb.enable_blob_files yes rocksdb.min_blob_size 4096"
check parity-kvrocks-no-blob "$(_parity_expect kvrocks relaxed | grep -c blob)" 0
check conf-kvrocks-wal-default "$(grep -c '^rocksdb.wal_' "$(write_server_conf kvrocks relaxed)")" 0
# --large: no WAL archive on either Kvrocks, and parity reads it back
W7_LARGE=1
for s in kvrocks kvrocks-blob; do
  conf=$(write_server_conf "$s" relaxed)
  check "conf-$s-wal-ttl" "$(grep -c '^rocksdb.wal_ttl_seconds 0$' "$conf")" 1
  check "conf-$s-wal-size" "$(grep -c '^rocksdb.wal_size_limit_mb 0$' "$conf")" 1
  check "parity-$s-wal" "$(_parity_expect "$s" relaxed | tail -2 | xargs)" "rocksdb.wal_ttl_seconds 0 rocksdb.wal_size_limit_mb 0"
done
check parity-blob-large "$(_parity_expect kvrocks-blob relaxed | grep -c blob)" 2
check conf-tideskv-no-wal "$(grep -c wal_ "$(write_server_conf tideskv relaxed)")" 0
# --large: no compression on any server (memtier's random data compresses ~3x under lz4), read back by parity
COMPRESSION=no
check conf-tideskv-compression-off "$(grep -c '^compression no$' "$(write_server_conf tideskv relaxed)")" 1
check parity-tideskv-compression-off "$(_parity_expect tideskv relaxed | grep -c '^compression no$')" 1
for s in kvrocks kvrocks-blob; do
  check "conf-$s-compression-off" "$(grep -c '^rocksdb.compression no$' "$(write_server_conf "$s" relaxed)")" 1
  check "parity-$s-compression-off" "$(_parity_expect "$s" relaxed | grep -c '^rocksdb.compression no$')" 1
done
COMPRESSION=lz4
check parity-compression-default "$(_parity_expect kvrocks relaxed | grep -c '^rocksdb.compression lz4$')" 1
check w7-sets-compression-off "$(declare -f run_large | grep -c 'COMPRESSION=no')" 1
W7_LARGE=0
BLOCK_CACHE_MB=2048 SERVER_WORKERS=4
wl_sizes large
W7_SWEEP=1
check w7-phases-sweep "$(w7_phase_list | xargs)" "R1 R4 R32 R32w16 M32"
W7_SWEEP=0
check w7-phases-no-sweep "$(w7_phase_list | xargs)" "R1 R4 R32 M32"
check cpu-count "$(cpu_count 8-11) $(cpu_count 0-3,8,10-11) $(cpu_count 5)" "4 7 1"
# logical x 2 + 8 GiB + fio's file: 32 x 2 + 8 + 8 = 80 GiB at large
SIZE=large FIO_SIZE=8g DISK_FREE_OVERRIDE=$((79 * 1024 * 1024 * 1024))
rc=0; disk_check 2>/dev/null || rc=$?
check disk-large-refuses-79g "$((rc != 0))" 1
DISK_FREE_OVERRIDE=$((81 * 1024 * 1024 * 1024)) rc=0; disk_check || rc=$?
check disk-large-ok-81g "$rc" 0
FIO_SIZE=1g DISK_FREE_OVERRIDE=$((74 * 1024 * 1024 * 1024)) rc=0; disk_check || rc=$?
check disk-large-fio-size-counts "$rc" 0
wl_sizes large-smoke; SIZE=large-smoke FIO_SIZE=1g DISK_FREE_OVERRIDE=$((10 * 1024 * 1024 * 1024)) rc=0
disk_check || rc=$?
check disk-large-smoke-ok "$rc" 0
unset DISK_FREE_OVERRIDE FIO_SIZE
check size-bytes "$(size_bytes 8g) $(size_bytes 512m) $(size_bytes 4096) $(size_bytes 2K)" "8589934592 536870912 4096 2048"
rc=0; size_bytes 8x >/dev/null 2>&1 || rc=$?
check size-bytes-bad "$((rc != 0))" 1
SIZE=quick; wl_sizes quick
# the kvrocks-blob DBSIZE cache needs the same DBSIZE SCAN as kvrocks
check load-kvrocks-glob "$(declare -f load_check | grep -c 'SERVER_NAME == kvrocks\*')" 2
# load_all checks through load_check, whose default verify list is wl_verify_cmds
check load-all-calls-check "$(declare -f load_all | grep -c '^ *load_check || return 1')" 1
# shellcheck disable=SC2016  # the literal text of run.sh
check load-check-default "$(declare -f load_check | grep -c 'verify=${1:-wl_verify_cmds}')" 1
# a W7 memtier container carries $TS so bench_cleanup's name filter finds it
# shellcheck disable=SC2016  # the literal text of run.sh
check w7-memtier-name "$(declare -f run_w7_phase | grep -c 'mc="tideskv-bench-memtier-$TS-w7-')" 1
check fio-pinned "$([[ $FIO_IMAGE == *@sha256:* ]] && echo yes)" yes
# a failed page-cache drop is detected (the container's find fails with the dd) and recorded
check evict-plus "$(grep -c "sh '{}' +" "$HERE/lib.sh")" 1
# shellcheck disable=SC2016  # the literal text of lib.sh
check evict-flag "$(declare -f start_server | grep -c 'evict_cache "$data" || EVICT_FAILED=1')" 1
check w7-markers "$(declare -f run_w7_phase | grep -c "M start\|M end")" 2
check w7-sync-before-io0 "$(declare -f run_w7_phase | grep -B6 'base.io0' | grep -c '^ *sync;\?$')" 1
# shellcheck disable=SC2016  # the literal text of run.sh
check w7-run-args-checked "$(declare -f run_w7_phase | grep -c 'wl_run_w7 "$phase" _run_cb || return')" 1
check w7-io2-after-settle "$(declare -f run_w7_phase | grep -A1 'settle-after' | grep -c 'io2')" 1

rc=0; bash "$HERE/run.sh" --no-w7-sweep >/dev/null 2>"$T/err" || rc=$?
check no-sweep-needs-large "$rc $(grep -c 'needs --large' "$T/err")" "2 1"
# M6: a missing option value is an error, and --help shows the defaults
rc=0; bash "$HERE/run.sh" --out >/dev/null 2>"$T/err" || rc=$?
check out-missing-value "$rc $(grep -c 'needs a value' "$T/err")" "1 1"
rc=0; bash "$HERE/run.sh" --relaxed-mode >/dev/null 2>"$T/err" || rc=$?
check mode-missing-value "$rc $(grep -c 'needs a value' "$T/err")" "1 1"
help=$(bash "$HERE/run.sh" --help)
check help-defaults "$(has "$help" "WARMUP/DURATION 60/180") $(has "$help" "CLIENTS 50") $(has "$help" "QUIESCE_MAX 600")" "yes yes yes"
# T3: --dirty builds untracked files too
check dirty-untracked "$(grep -c 'ls-files -z --cached --others --exclude-standard' "$HERE/lib.sh")" 1

if command -v shellcheck >/dev/null; then
  shellcheck -x -P "$HERE" "$HERE/workloads.sh" "$HERE/test_workloads.sh" "$HERE/lib.sh" "$HERE/run.sh" && echo "ok   shellcheck" || fail=1
else
  echo "skip shellcheck (not installed)"
fi
bash -n "$HERE/workloads.sh" || fail=1
bash -n "$HERE/lib.sh" "$HERE/run.sh" || fail=1
exit $fail
