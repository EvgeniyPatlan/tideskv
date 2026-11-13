#!/usr/bin/env bash
# Use-case suite case definitions U1-U8 and F1 (spec 2026-11-12 §4). Sourced by uc_run.sh; pure functions, no Docker.
# Callbacks receive memtier arguments as separate words. The runner appends --test-time (UC_WARMUP / UC_DURATION, or
# the phase-specific globals below), so phase arguments carry traffic shape only.
# Globals set by uc_sizes (read by the runner):
#   UC_REPS                       repetitions per phase (3 full, 1 quick)
#   UC_WARMUP UC_DURATION         closed-loop warm-up / timed seconds (60/180, quick 10/30)
#   U1_KEYS U3_KEYS U4_KEYS U6_KEYS U7_KEYS   key counts; U5 keys per size come from uc_u5_keys
#   U2_RATE_PER_CONN U2_EXPIRY    U2 per-connection SET/GET rate and --expiry-range value
#   UC_U2_WRITE_S UC_U2_IDLE_S UC_U2_DU_EVERY   U2 churn seconds, idle seconds, du sampling interval
#   UC_U3_CLOSED_S UC_U3_OPEN_S   U3 closed-loop and per-open-phase seconds
#   UC_U6_S                       U6 seconds per connection-count phase
#   UC_U7_TIMES UC_U7_ACKS        U7 repeats per phase, and the space-separated acked-write counts before the signal
#   UC_U7_ACK_TIMEOUT             longest wait in seconds for a U7 point's acked-write count (ERR when not reached)
#   UC_COMPACT_WAIT               longest wait in seconds for U1's Kvrocks COMPACT to finish
# UC_RATE (set by the runner) is the per-connection --rate-limiting of the open-loop phases of U1 and U3.
# shellcheck disable=SC2034  # globals are read by uc_run.sh and the test
HERE_UC=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source-path=SCRIPTDIR source=workloads.sh
source "$HERE_UC/workloads.sh"

# The relaxed level (U1-U5, U8): TidesKV sync-mode none, which lib.sh reads from RELAXED (run.sh's --relaxed-mode)
RELAXED=none
UC_THREADS=4
UC_CLIENTS=50
UC_GIB=$((1024 * 1024 * 1024))
UC_WAL_ALLOWANCE=$((16 * UC_GIB))
UC_SLACK=$((4 * UC_GIB))

uc_sizes() {
  case $1 in
    full)
      UC_REPS=3 UC_WARMUP=60 UC_DURATION=180
      U1_KEYS=20000000 U3_KEYS=20000000 U4_KEYS=5000000 U6_KEYS=1000000 U7_KEYS=2000000
      U2_RATE_PER_CONN=200 U2_EXPIRY=120-1200 UC_U2_WRITE_S=3600 UC_U2_IDLE_S=1800 UC_U2_DU_EVERY=60
      UC_U3_CLOSED_S=3600 UC_U3_OPEN_S=600 UC_U6_S=60 UC_U7_TIMES=5 UC_U7_ACK_DIV=1 UC_U7_ACK_TIMEOUT=900
      U5_KEYS_1K=16000000 U5_KEYS_4K=4000000 U5_KEYS_16K=1000000 UC_COMPACT_WAIT=7200 ;;
    quick)
      UC_REPS=1 UC_WARMUP=10 UC_DURATION=30
      U1_KEYS=2000000 U3_KEYS=1000000 U4_KEYS=250000 U6_KEYS=1000000 U7_KEYS=200000
      U2_RATE_PER_CONN=50 U2_EXPIRY=30-300 UC_U2_WRITE_S=600 UC_U2_IDLE_S=300 UC_U2_DU_EVERY=15
      UC_U3_CLOSED_S=300 UC_U3_OPEN_S=60 UC_U6_S=15 UC_U7_TIMES=1
      # U7 points at a tenth of the full counts (1271 6355 11439 acked writes)
      UC_U7_ACK_DIV=10 UC_U7_ACK_TIMEOUT=300
      # 0.5 GiB logical per value size
      U5_KEYS_1K=524288 U5_KEYS_4K=131072 U5_KEYS_16K=32768 UC_COMPACT_WAIT=600 ;;
    *) echo "uc_sizes: unknown mode $1" >&2; return 2 ;;
  esac
  UC_U2_CLIENTS=25
  UC_U7_ACKS=$(_uc_u7_acks)
}

# U7 load points (spec section 4: "kill at the same load point for both"): the signal goes after N acknowledged writes,
# the same N on both servers, at about 10, 50 and 90% of the 256 MiB memtable (memtable-size 268435456 / Kvrocks
# rocksdb.write_buffer_size 256 in lib.sh). An entry is the writer's value (UC_U7_VALUE bytes, `<seq>:` padded) plus 64 B
# for the key (about 30 B: ack-r<rep>-<sig>-<N>-<run>-<thread>:<seq>) and the engines' per-entry memtable overhead.
# With the 2 KiB value the overhead is about 3% of an entry, so the fill estimate does not hinge on either engine's
# encoding, and the 90% point is about 114k writes: 40 s to 4 min at the 500-3200 strict acks/s of the quick run
# (a value of the seq alone, about 60 B per entry, would need 3.9M writes, over 20 minutes per point).
# The fill counts the writer's writes only; the preload before the first point, and what each restart's WAL replay
# leaves in the memtable, are not counted (the same for both servers).
UC_U7_VALUE=2048
UC_U7_ENTRY=$((UC_U7_VALUE + 64))
UC_U7_MEMTABLE=$((256 << 20))
_uc_u7_acks() {
  local p out=()
  for p in 10 50 90; do out+=($((UC_U7_MEMTABLE * p / 100 / UC_U7_ENTRY / UC_U7_ACK_DIV))); done
  echo "${out[*]}"
}

# Cache accounting (spec §4 U1, note 2026-11-12-uc-cache-accounting): TidesDB charges btree nodes (index) and
# pr_filter (bloom) blobs to its block cache, so Kvrocks keeps its default rocksdb.cache_index_and_filter_blocks yes
# and needs no extra line. "charged" goes into meta.json (cache_accounting).
UC_CACHE_ACCOUNTING=charged
UC_CACHE_LINES=()

# Background-debt INFO fields (spec §4 U3) the report reads from each cell's .info0/.info1 (full INFO), as found in a
# live INFO dump on 2026-11-12. Kvrocks 2.17 prints per-column-family fields as name[cf] (estimate_pending_compaction_
# bytes[default], ...; the report sums them); is_compacting is yes only while a manual COMPACT runs. TidesKV has no
# compaction or flush queue length and no vlog-garbage field: its debt shows as the memtable queue, write throttling
# and stall time, and the background compaction counters.
UC_DEBT_FIELDS_kvrocks=(estimate_pending_compaction_bytes compaction_pending num_running_compactions
                        memtable_flush_pending num_running_flushes is_compacting)
UC_DEBT_FIELDS_tideskv=(storage_memtable_queue_bytes storage_writes_throttled storage_writes_blocked
                        storage_write_stall_us background_compactions background_compaction_failures)

# Extra "key value" config lines of SERVER, one per line (the runner reads them into UC_EXTRA_CONF; lib.sh appends
# them to the Kvrocks conf and to _parity_expect, skipping lines already there, so kvrocks-blob's lines, which
# write_server_conf adds itself, are not doubled). Needs KVROCKS_BLOB_CONF from lib.sh.
uc_conf_lines() {
  case $1 in
    tideskv) ;;
    kvrocks) ((${#UC_CACHE_LINES[@]})) && printf '%s\n' "${UC_CACHE_LINES[@]}" ;;
    kvrocks-cron)
      echo "compaction-checker-cron * * * * *"
      ((${#UC_CACHE_LINES[@]})) && printf '%s\n' "${UC_CACHE_LINES[@]}" ;;
    kvrocks-blob)
      printf '%s\n' "${KVROCKS_BLOB_CONF[@]}"
      ((${#UC_CACHE_LINES[@]})) && printf '%s\n' "${UC_CACHE_LINES[@]}" ;;
    *) echo "uc_conf_lines: unknown server $1" >&2; return 2 ;;
  esac
  return 0
}

uc_cases() { echo "U1 U2 U3 U4 U5 U6 U7 U8 F1"; }

uc_servers() {
  case $1 in
    U2) echo "tideskv kvrocks kvrocks-cron" ;;
    U5) echo "tideskv kvrocks kvrocks-blob" ;;
    U1|U3|U4|U6|U7|U8|F1) echo "tideskv kvrocks" ;;
    *) echo "uc_servers: unknown case $1" >&2; return 2 ;;
  esac
}

uc_level() {
  case $1 in
    U6|U7) echo strict ;;
    U1|U2|U3|U4|U5|U8|F1) echo relaxed ;;
    *) echo "uc_level: unknown case $1" >&2; return 2 ;;
  esac
}

# Keys and value size of one U5 phase (v1k, v4k, v16k).
uc_u5_keys() {
  case $1 in
    v1k) echo "$U5_KEYS_1K 1024" ;;
    v4k) echo "$U5_KEYS_4K 4096" ;;
    v16k) echo "$U5_KEYS_16K 16384" ;;
    *) echo "uc_u5_keys: unknown phase $1" >&2; return 2 ;;
  esac
}

# Logical bytes a case keeps on disk (key + per-entry overhead as in wl_logical_bytes). U8 starts no server: it reads
# U1's and U3's files.
_uc_logical() {
  local k v
  case $1 in
    U1) echo $((U1_KEYS * (1024 + 12))) ;;
    # live set of the churn: SET rate x mean TTL (SET is half of the ops)
    U2) local mean=${U2_EXPIRY%-*}
        mean=$(((mean + ${U2_EXPIRY#*-}) / 2))
        echo $((UC_THREADS * UC_U2_CLIENTS * U2_RATE_PER_CONN * mean * (1024 + 12) / 2)) ;;
    U3) echo $((U3_KEYS * (200 + 12))) ;;
    U4) echo $((U4_KEYS * 20 * (50 + 10))) ;;
    # each size is loaded and removed in turn, so the largest one counts
    U5) local p max=0
        for p in v1k v4k v16k; do read -r k v <<<"$(uc_u5_keys "$p")"; ((k * (v + 12) > max)) && max=$((k * (v + 12))); done
        echo "$max" ;;
    U6) echo $((U6_KEYS * (100 + 12))) ;;
    # the preload, plus every point's acked writes: the data dir is kept from point to point
    U7) local n w=0
        for n in $UC_U7_ACKS; do w=$((w + n)); done
        echo $((U7_KEYS * (100 + 12) + UC_U7_TIMES * 2 * w * UC_U7_ENTRY)) ;;
    U8|F1) echo 0 ;;
    *) echo "uc_disk_need: unknown case $1" >&2; return 2 ;;
  esac
}

# Bytes free a case needs (ruling F1-crit, replacing R6's "LZ4 compresses ~3x", which the quick run contradicts). Only
# one server's data is on disk at a time (each pass starts from an emptied data dir), so the need is the larger of the
# two servers' needs, not their sum:
#   TidesKV: logical x 1.4 + UC_SLACK
#   Kvrocks: logical x 1.6 + UC_WAL_ALLOWANCE + UC_SLACK
#   logical x 1.4/1.1         the data on disk after the load. memtier's --random-data does not compress (U1 load is 1 KiB
#                             values of random bytes): in the quick run build/bench/uc-quick (du-after-load: allocated bytes
#                             excluding the WAL archive, over keys x (value + 12)) U1 TidesKV was 1.33x and Kvrocks 1.05x;
#                             U5 1/4/16 KiB TidesKV 1.48/1.25/1.00x, Kvrocks 1.41/1.43/1.56x at 0.5 GiB per size, where
#                             fixed per-file overheads weigh more than at full size.
#   Kvrocks + logical / 2     the transient extra space of its full COMPACT (U1) while old and new SSTs coexist
#                             (1.1 + 0.5 = 1.6)
#   UC_WAL_ALLOWANCE          Kvrocks' WAL archive (stock wal_size_limit_mb 16384 caps it at 16 GiB)
#   UC_SLACK                  4 GiB for logs, the fio files and the filesystem
# U8 and F1 (logical 0) still ask for 20 GiB (the Kvrocks term).
uc_disk_need() {
  local logical t k
  logical=$(_uc_logical "$1") || return
  t=$((logical * 14 / 10 + UC_SLACK))
  k=$((logical * 16 / 10 + UC_WAL_ALLOWANCE + UC_SLACK))
  echo $((t > k ? t : k))
}

uc_phases() {
  local t
  case $1 in
    U1) echo "closed-gauss closed-uniform open25 open50 open75" ;;
    U2) echo "churn idle" ;;
    U3) echo "age closed open25 open50 open75" ;;
    U4) echo "mix" ;;
    U5) echo "v1k v4k v16k" ;;
    U6) echo "c1 c8 c64 c256" ;;
    U7) local out=()
        for t in $UC_U7_ACKS; do out+=("term-$t" "kill-$t"); done
        echo "${out[*]}" ;;
    U8) echo "report-only" ;;
    F1) echo "matrix" ;;
    *) echo "uc_phases: unknown case $1" >&2; return 2 ;;
  esac
}

# Closed-loop shape: 4 threads x 50 clients, pipeline 1, distinct seeds.
_uc_conns() { echo "--threads=$UC_THREADS" "--clients=$UC_CLIENTS" "--pipeline=1" "${MEMTIER_RUN_FLAGS[@]}"; }

# String load of N keys of SIZE bytes under PREFIX (same _seq pattern as wl_load).
_uc_str_load() {
  local cb=$1 prefix=$2 keys=$3 size=$4
  # shellcheck disable=SC2046
  "$cb" "--ratio=1:0" "--data-size=$size" "--random-data" "--key-prefix=$prefix" $(_seq "$keys")
}

# Load CASE through CB. U5 takes an optional PHASE (v1k|v4k|v16k) and loads all three sizes in turn without one.
uc_load() {
  local c=$1 cb=$2 p k v args=() f
  case $c in
    U1) _uc_str_load "$cb" "u1:" "$U1_KEYS" 1024 ;;
    U3) _uc_str_load "$cb" "u3:" "$U3_KEYS" 200 ;;
    U4)
      for ((f = 1; f <= 20; f++)); do args+=("f$f" "__data__"); done
      # shellcheck disable=SC2046
      "$cb" "--command=HSET __key__ ${args[*]}" "--data-size=50" "--random-data" "--key-prefix=u4:" \
        $(_cseq "$U4_KEYS") ;;
    U5)
      for p in ${3:-v1k v4k v16k}; do
        read -r k v <<<"$(uc_u5_keys "$p")" || return
        _uc_str_load "$cb" "u5:" "$k" "$v" || return
      done ;;
    U6) _uc_str_load "$cb" "u6:" "$U6_KEYS" 100 ;;
    U7) _uc_str_load "$cb" "u7:" "$U7_KEYS" 100 ;;
    U2|U8|F1) ;;
    *) echo "uc_load: unknown case $c" >&2; return 2 ;;
  esac
}

# Gaussian over [1, keys]: median keys/2, stddev keys/20.
_uc_gauss() { echo "--key-pattern=G:G" "--key-minimum=1" "--key-maximum=$1" "--key-median=$(($1 / 2))" \
  "--key-stddev=$(($1 / 20))"; }

_uc_u1() {
  local ph=$1 cb=$2
  # shellcheck disable=SC2046
  case $ph in
    closed-gauss)
      "$cb" "--ratio=1:10" "--data-size=1024" "--random-data" "--key-prefix=u1:" $(_uc_gauss "$U1_KEYS") $(_uc_conns) ;;
    closed-uniform)
      "$cb" "--ratio=1:10" "--data-size=1024" "--random-data" "--key-prefix=u1:" "--key-pattern=R:R" \
        $(_random "$U1_KEYS") $(_uc_conns) ;;
    open25|open50|open75)
      "$cb" "--ratio=1:10" "--data-size=1024" "--random-data" "--key-prefix=u1:" $(_uc_gauss "$U1_KEYS") $(_uc_conns) \
        "--rate-limiting=$UC_RATE" ;;
    *) echo "uc_phase: unknown U1 phase $ph" >&2; return 2 ;;
  esac
}

_uc_u3() {
  local ph=$1 cb=$2
  # shellcheck disable=SC2046
  case $ph in
    # two overwrites of the loaded set: 2 x U3_KEYS random pipelined SETs split over 4 x 8 clients
    age)
      "$cb" "--ratio=1:0" "--data-size=200" "--random-data" "--key-prefix=u3:" "--key-pattern=R:R" \
        $(_random "$U3_KEYS") "--requests=$((2 * U3_KEYS / 32))" "--pipeline=32" "--threads=4" "--clients=8" \
        "${MEMTIER_RUN_FLAGS[@]}" ;;
    closed)
      "$cb" "--ratio=4:1" "--data-size=200" "--random-data" "--key-prefix=u3:" "--key-pattern=R:R" \
        $(_random "$U3_KEYS") $(_uc_conns) ;;
    open25|open50|open75)
      "$cb" "--ratio=4:1" "--data-size=200" "--random-data" "--key-prefix=u3:" "--key-pattern=R:R" \
        $(_random "$U3_KEYS") $(_uc_conns) "--rate-limiting=$UC_RATE" ;;
    *) echo "uc_phase: unknown U3 phase $ph" >&2; return 2 ;;
  esac
}

_uc_u4() {
  # shellcheck disable=SC2046
  "$2" "--command=HGET __key__ f1" "--command-ratio=10" "--command-key-pattern=R" \
    "--command=HSET __key__ f1 __data__" "--command-ratio=6" "--command-key-pattern=R" \
    "--command=HGETALL __key__" "--command-ratio=3" "--command-key-pattern=R" \
    "--command=HDEL __key__ f1" "--command-ratio=1" "--command-key-pattern=R" \
    "--data-size=50" "--random-data" "--key-prefix=u4:" $(_random "$U4_KEYS") $(_uc_conns)
}

_uc_u6() {
  local th cl
  case $1 in
    c1) th=1 cl=1 ;;
    c8) th=1 cl=8 ;;
    c64) th=4 cl=16 ;;
    c256) th=4 cl=64 ;;
    *) echo "uc_phase: unknown U6 phase $1" >&2; return 2 ;;
  esac
  # shellcheck disable=SC2046
  "$2" "--ratio=1:0" "--data-size=100" "--random-data" "--key-prefix=u6:" "--key-pattern=R:R" $(_random "$U6_KEYS") \
    "--threads=$th" "--clients=$cl" "--pipeline=1" "${MEMTIER_RUN_FLAGS[@]}"
}

# Memtier arguments of one phase through CB. Phases without memtier traffic (U2 idle, U7, U8, F1) call nothing.
uc_phase() {
  local c=$1 ph=$2 cb=$3 k v
  case $c in
    U1) _uc_u1 "$ph" "$cb" ;;
    U2)
      case $ph in
        churn)
          # shellcheck disable=SC2046
          "$cb" "--ratio=1:1" "--data-size=1024" "--random-data" "--key-prefix=u2:" "--key-pattern=R:R" \
            "--key-minimum=1" "--key-maximum=1000000000" "--expiry-range=$U2_EXPIRY" "--threads=$UC_THREADS" \
            "--clients=$UC_U2_CLIENTS" "--pipeline=1" "--rate-limiting=$U2_RATE_PER_CONN" "${MEMTIER_RUN_FLAGS[@]}" ;;
        idle) ;;
        *) echo "uc_phase: unknown U2 phase $ph" >&2; return 2 ;;
      esac ;;
    U3) _uc_u3 "$ph" "$cb" ;;
    U4) [[ $ph == mix ]] || { echo "uc_phase: unknown U4 phase $ph" >&2; return 2; }; _uc_u4 "$ph" "$cb" ;;
    U5)
      read -r k v <<<"$(uc_u5_keys "$ph")" || return
      # shellcheck disable=SC2046
      "$cb" "--ratio=1:4" "--data-size=$v" "--random-data" "--key-prefix=u5:" "--key-pattern=R:R" $(_random "$k") \
        $(_uc_conns) ;;
    U6) _uc_u6 "$ph" "$cb" ;;
    # the ack-writer is a separate tool: U7's per-phase logic lives in the runner
    U7|U8|F1) ;;
    *) echo "uc_phase: unknown case $c" >&2; return 2 ;;
  esac
}
