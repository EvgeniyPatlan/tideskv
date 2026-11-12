#!/usr/bin/env bash
# memtier arguments for the Kvrocks common cases C1-C8. Sourced by run.sh after workloads.sh, whose _seq, _cseq,
# _random and MEMTIER_RUN_FLAGS it reuses; callbacks receive memtier arguments as separate words.
# wc_load owns --threads/--clients (1x1 for --command loads, see workloads.sh); wc_run leaves them to the caller,
# except for the C8 parts, which are fixed by the case.
# memtier 2.1.4: every __key__ in a --command draws its own key; --key-prefix applies to all of them.
# shellcheck disable=SC2034  # globals are read by run.sh and the test
CASES="C1 C2 C3 C4 C5 C6 C7 C8"
CASE_DATASETS="cache cache_exp sessions counters sets boards feeds batch"
CASE_RATE=10000
C3_HSTATS=10
C6_VALUE=100
C7_VALUE=100

wc_sizes() {
  case $1 in
    quick) C1_KEYS=200000 C1_EXPIRE_S=30 C2_KEYS=100000 C3_KEYS=100000 C4_SETS=10 C4_MEMBERS=20000
           C5_BOARDS=5 C5_MEMBERS=100000 C6_LISTS=20000 C6_AVG=200 C7_KEYS=1000000 CASE_REPEATS=1 ;;
    medium) C1_KEYS=3000000 C1_EXPIRE_S=120 C2_KEYS=2000000 C3_KEYS=1000000 C4_SETS=100 C4_MEMBERS=100000
            C5_BOARDS=20 C5_MEMBERS=1000000 C6_LISTS=20000 C6_AVG=500 C7_KEYS=10000000 CASE_REPEATS=3 ;;
    *) echo "wc_sizes: unknown size $1" >&2; return 2 ;;
  esac
  C1_VALUE=2048 C2_FIELDS=30 C2_VALUE=50
}

# _cseq for a range starting at $1 instead of 1
_cseq_from() {
  local out
  out=$(_cseq "$2")
  echo "${out/--key-minimum=1 /--key-minimum=$1 }"
}

wc_load() {
  local ds=$1 cb=$2 i f split=$((C1_KEYS * 8 / 10)) fields=()
  case $ds in
    cache)
      # shellcheck disable=SC2046
      "$cb" "--command=SET __key__ __data__" "--data-size=$C1_VALUE" "--random-data" "--key-prefix=c:" \
        $(_cseq "$split") || return ;;
    cache_exp)
      # shellcheck disable=SC2046
      "$cb" "--command=SET __key__ __data__ EX $C1_EXPIRE_S" "--data-size=$C1_VALUE" "--random-data" \
        "--key-prefix=c:" $(_cseq_from $((split + 1)) "$C1_KEYS") || return ;;
    sessions)
      for ((f = 1; f <= C2_FIELDS; f++)); do fields+=("f$f" "__data__"); done
      # shellcheck disable=SC2046
      "$cb" "--command=HSET __key__ ${fields[*]}" "--data-size=$C2_VALUE" "--random-data" \
        "--key-prefix=sess:" $(_cseq "$C2_KEYS") || return ;;
    counters)
      # shellcheck disable=SC2046
      "$cb" "--command=SET __key__ 0" "--key-prefix=ctr:" $(_cseq "$C3_KEYS") || return ;;
    sets)
      for ((i = 1; i <= C4_SETS; i++)); do
        # shellcheck disable=SC2046
        "$cb" "--command=SADD set:$i __key__" "--key-prefix=m:" $(_cseq "$C4_MEMBERS") || return
      done ;;
    boards)
      for ((i = 1; i <= C5_BOARDS; i++)); do
        # shellcheck disable=SC2046
        "$cb" "--command=ZADD lb:$i __key__ __key__" "--key-prefix=" $(_cseq "$C5_MEMBERS") || return
      done ;;
    feeds)
      # R, not P: each LPUSH lands on a random list, so C6_LISTS lists grow to ~C6_AVG elements
      "$cb" "--command=LPUSH __key__ __data__" "--data-size=$C6_VALUE" "--random-data" "--key-prefix=feed:" \
        "--command-key-pattern=R" "--key-minimum=1" "--key-maximum=$C6_LISTS" "--requests=$((C6_LISTS * C6_AVG))" \
        "--pipeline=32" "--threads=1" "--clients=1" || return ;;
    batch)
      # shellcheck disable=SC2046
      "$cb" "--ratio=1:0" "--data-size=$C7_VALUE" "--random-data" "--key-prefix=mk:" $(_seq "$C7_KEYS") || return ;;
    *) echo "wc_load: unknown dataset $ds" >&2; return 2 ;;
  esac
}

# Appends one --command/--command-ratio/--command-key-pattern triple to _wc_args: _wc_add <ratio> <pattern> <command>
_wc_add() { _wc_args+=("--command=$3" "--command-ratio=$1" "--command-key-pattern=$2"); }

_wc_c1() {
  _wc_args=(); _wc_add 18 R "GET __key__"
  _wc_add 1 R "SET __key__ __data__ EX 60"; _wc_add 1 R "SET __key__ __data__ EX 3600"
  # shellcheck disable=SC2046
  "$1" "${_wc_args[@]}" "--data-size=$C1_VALUE" "--random-data" "--key-prefix=c:" $(_random "$C1_KEYS")
}

_wc_c2() {
  _wc_args=(); _wc_add 6 R "HGETALL __key__"; _wc_add 3 R "HSET __key__ f1 __data__"; _wc_add 1 R "EXPIRE __key__ 3600"
  # shellcheck disable=SC2046
  "$1" "${_wc_args[@]}" "--data-size=$C2_VALUE" "--random-data" "--key-prefix=sess:" $(_random "$C2_KEYS")
}

# Gaussian hot keys: INCRBY and GET on ctr:, plus one HINCRBY per hstats:<i> hash.
# memtier 2.1.4 rejects --key-stddev/--key-median together with --command (it checks --key-pattern, which --command
# forbids), so the skew comes from the range instead: with the default stddev = range/6, a range of 6 sigma centred
# on C3_KEYS/2 gives sigma = C3_KEYS/100.
_wc_c3() {
  local i sigma=$((C3_KEYS / 100)) mid=$((C3_KEYS / 2)); _wc_args=()
  _wc_add 15 G "INCRBY __key__ 1"
  for ((i = 1; i <= C3_HSTATS; i++)); do _wc_add 1 G "HINCRBY hstats:$i __key__ 1"; done
  _wc_add 25 G "GET __key__"
  "$1" "${_wc_args[@]}" "--key-prefix=ctr:" "--key-minimum=$((mid - 3 * sigma))" "--key-maximum=$((mid + 3 * sigma))"
}

_wc_c4() {
  local i; _wc_args=()
  for ((i = 1; i <= C4_SETS; i++)); do
    _wc_add 4 R "SADD set:$i __key__"; _wc_add 15 R "SISMEMBER set:$i __key__"; _wc_add 1 R "SSCAN set:$i 0 COUNT 100"
  done
  # shellcheck disable=SC2046
  "$1" "${_wc_args[@]}" "--key-prefix=m:" $(_random $((2 * C4_MEMBERS)))
}

_wc_c5() {
  local i; _wc_args=()
  for ((i = 1; i <= C5_BOARDS; i++)); do
    _wc_add 4 R "ZINCRBY lb:$i 1 __key__"; _wc_add 3 R "ZREVRANGE lb:$i 0 9"
    _wc_add 2 R "ZREVRANK lb:$i __key__"; _wc_add 1 R "ZSCORE lb:$i __key__"
  done
  # shellcheck disable=SC2046
  "$1" "${_wc_args[@]}" "--key-prefix=" $(_random $((2 * C5_MEMBERS)))
}

_wc_c6() {
  _wc_args=(); _wc_add 2 R "LPUSH __key__ __data__"; _wc_add 1 R "LTRIM __key__ 0 999"; _wc_add 7 R "LRANGE __key__ 0 49"
  # shellcheck disable=SC2046
  "$1" "${_wc_args[@]}" "--data-size=$C6_VALUE" "--random-data" "--key-prefix=feed:" $(_random "$C6_LISTS")
}

# each __key__ draws its own key: MGET reads 20 keys, MSET writes 10
_wc_c7() {
  local mget="MGET" mset="MSET" i
  for ((i = 0; i < 20; i++)); do mget+=" __key__"; done
  for ((i = 0; i < 10; i++)); do mset+=" __key__ __data__"; done
  _wc_args=(); _wc_add 9 R "$mget"; _wc_add 1 R "$mset"
  # shellcheck disable=SC2046
  "$1" "${_wc_args[@]}" "--data-size=$C7_VALUE" "--random-data" "--key-prefix=mk:" $(_random "$C7_KEYS")
}

wc_run() {
  case $1 in
    C1|C2|C3|C4|C5|C6|C7) "_wc_${1,,}" "$2" ;;
    C8) echo "wc_run: C8 is a mix of parts, use wc_c8_parts" >&2; return 2 ;;
    *) echo "wc_run: unknown case $1" >&2; return 2 ;;
  esac
}

# C8 runs C1, C2 and C5 at 2 threads x 16 clients next to a rate-limited full-keyspace SCAN
_wc_grab() { _wc_grabbed=("$@"); }
wc_c8_parts() {
  local cb=$1 part
  for part in c1 c2 c5; do
    wc_run "${part^^}" _wc_grab || return
    "$cb" "$part" "${_wc_grabbed[@]}" "--threads=2" "--clients=16" || return
  done
  "$cb" scan "--command=SCAN 0 MATCH c:1* COUNT 1000" "--command-ratio=1" "--command-key-pattern=R" \
    "--threads=1" "--clients=5" "--rate-limiting=1"
}

# memtier rate-limits per connection, so the case's total rate is spread over threads x clients connections
wc_rate_per_conn() {
  local r=$((CASE_RATE / ($1 * $2)))
  echo $((r < 1 ? 1 : r))
}

wc_probe() {
  local v
  v=$(printf 'x%.0s' $(seq 1 "$C1_VALUE"))
  case $1 in
    C1) echo "SET c:1 $v EX 60"; echo "GET c:1" ;;
    C2) echo "HSET sess:1 f1 $v"; echo "HGETALL sess:1" ;;
    C3) echo "INCRBY ctr:1 1"; echo "GET ctr:1" ;;
    C4) echo "SADD set:1 1"; echo "SISMEMBER set:1 1" ;;
    C5) echo "ZINCRBY lb:1 1 1"; echo "ZSCORE lb:1 1" ;;
    C6) echo "LPUSH feed:1 $v"; echo "LRANGE feed:1 0 49" ;;
    C7) echo "MSET mk:1 $v mk:2 $v"; echo "MGET mk:1 mk:2" ;;
    *) echo "wc_probe: unknown case $1" >&2; return 2 ;;
  esac
}

wc_verify_cmds() {
  # 99% of the planned count: memtier skips edge ids
  echo "$(( (C1_KEYS + C2_KEYS + C3_KEYS + C4_SETS + C5_BOARDS + C6_LISTS + C7_KEYS) * 99 / 100 )) DBSIZE"
  echo "$C2_FIELDS HLEN sess:1"
  echo "$(( C4_MEMBERS * 99 / 100 )) SCARD set:1"
  echo "$(( C5_MEMBERS * 99 / 100 )) ZCARD lb:1"
  echo "1 LLEN feed:1"
}

# rough per-entry sizes: key + value + a few bytes of structure
wc_logical_bytes() {
  echo $(( C1_KEYS * (C1_VALUE + 10) + C2_KEYS * (12 + C2_FIELDS * (C2_VALUE + 6)) + C3_KEYS * 16 \
         + C4_SETS * C4_MEMBERS * 12 + C5_BOARDS * C5_MEMBERS * 20 + C6_LISTS * C6_AVG * C6_VALUE \
         + C7_KEYS * (C7_VALUE + 12) ))
}
