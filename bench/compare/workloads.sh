#!/usr/bin/env bash
# memtier arguments for the comparison workloads W1-W6 (spec 2026-10-18 §6, plan rulings 1-2).
# Sourced by run.sh; callbacks receive memtier arguments as separate words.
# shellcheck disable=SC2034  # globals are read by run.sh and the test
DATASETS="strings hashes zsets lists bighashes"
WORKLOADS="W1 W2 W3 W4 W5 W6"
VALUE_SIZE=100

wl_sizes() {
  case $1 in
    full) S_KEYS=180000000 H_COUNT=50 H_FIELDS=10000 Z_COUNT=50 Z_MEMBERS=100000
          L_COUNT=50 L_ELEMS=100000 BH_COUNT=30000 BH_FIELDS=1000 W6_DEL_RATE=100 ;;
    quick) S_KEYS=4000000 H_COUNT=5 H_FIELDS=10000 Z_COUNT=5 Z_MEMBERS=10000
           L_COUNT=5 L_ELEMS=10000 BH_COUNT=1000 BH_FIELDS=1000 W6_DEL_RATE=20 ;;
    *) echo "wl_sizes: unknown size $1" >&2; return 2 ;;
  esac
}

# Sequential load of every key in [1, max]: P:P with allkeys splits the range across threads x clients.
_seq() { echo "--key-pattern=P:P" "--key-minimum=1" "--key-maximum=$1" "--requests=allkeys" "--pipeline=32"; }

wl_load() {
  local ds=$1 cb=$2 i
  case $ds in
    strings)
      # shellcheck disable=SC2046
      "$cb" "--ratio=1:0" "--data-size=$VALUE_SIZE" "--key-prefix=s:" $(_seq "$S_KEYS") ;;
    hashes)
      for ((i = 1; i <= H_COUNT; i++)); do
        # shellcheck disable=SC2046
        "$cb" "--command=HSET h:$i __key__ __data__" "--command-key-pattern=P" "--data-size=$VALUE_SIZE" \
          "--key-prefix=f:" $(_seq "$H_FIELDS")
      done ;;
    zsets)
      for ((i = 1; i <= Z_COUNT; i++)); do
        # shellcheck disable=SC2046
        "$cb" "--command=ZADD z:$i __key__ m__key__" "--command-key-pattern=P" "--key-prefix=" $(_seq "$Z_MEMBERS")
      done ;;
    lists)
      for ((i = 1; i <= L_COUNT; i++)); do
        # shellcheck disable=SC2046
        "$cb" "--command=LPUSH l:$i __data__" "--command-key-pattern=P" "--data-size=$VALUE_SIZE" $(_seq "$L_ELEMS")
      done ;;
    bighashes)
      # one hash per key bh:<n>, BH_FIELDS fields each, via HSET with BH_FIELDS field/value pairs
      local args=() f
      for ((f = 1; f <= BH_FIELDS; f++)); do args+=("f$f" "__data__"); done
      # shellcheck disable=SC2046
      "$cb" "--command=HSET __key__ ${args[*]}" "--command-key-pattern=P" "--data-size=$VALUE_SIZE" \
        "--key-prefix=bh:" $(_seq "$BH_COUNT") ;;
    *) echo "wl_load: unknown dataset $ds" >&2; return 2 ;;
  esac
}

_random() { echo "--key-minimum=1" "--key-maximum=$1"; }

wl_run() {
  local w=$1 cb=$2 i args=()
  case $w in
    W1|W6)
      # shellcheck disable=SC2046
      "$cb" "--ratio=1:1" "--data-size=$VALUE_SIZE" "--key-prefix=s:" "--key-pattern=R:R" $(_random "$S_KEYS") ;;
    W2)
      # shellcheck disable=SC2046
      "$cb" "--ratio=1:10" "--data-size=$VALUE_SIZE" "--key-prefix=s:" "--key-pattern=R:R" $(_random "$S_KEYS") ;;
    W3)
      for ((i = 1; i <= H_COUNT; i++)); do
        args+=("--command=HSET h:$i __key__ __data__" "--command-ratio=1" "--command-key-pattern=R"
               "--command=HGET h:$i __key__" "--command-ratio=1" "--command-key-pattern=R")
      done
      # shellcheck disable=SC2046
      "$cb" "${args[@]}" "--data-size=$VALUE_SIZE" "--key-prefix=f:" $(_random "$H_FIELDS") ;;
    W4)
      for ((i = 1; i <= Z_COUNT; i++)); do
        args+=("--command=ZADD z:$i __key__ m__key__" "--command-ratio=1" "--command-key-pattern=R"
               "--command=ZRANGEBYSCORE z:$i __key__ +inf LIMIT 0 10" "--command-ratio=1" "--command-key-pattern=R")
      done
      # shellcheck disable=SC2046
      "$cb" "${args[@]}" "--key-prefix=" $(_random "$Z_MEMBERS") ;;
    W5)
      for ((i = 1; i <= L_COUNT; i++)); do
        args+=("--command=LPUSH l:$i __data__" "--command-ratio=1" "--command=LPOP l:$i" "--command-ratio=1")
      done
      "$cb" "${args[@]}" "--data-size=$VALUE_SIZE" ;;
    *) echo "wl_run: unknown workload $w" >&2; return 2 ;;
  esac
}

wl_run_w6_del() {
  # shellcheck disable=SC2046
  "$1" "--command=DEL __key__" "--command-key-pattern=R" "--key-prefix=bh:" $(_random "$BH_COUNT") \
    "--threads=1" "--clients=1" "--rate-limiting=$W6_DEL_RATE"
}

wl_probe() {
  local v
  v=$(printf 'x%.0s' $(seq 1 "$VALUE_SIZE"))
  case $1 in
    W1|W2|W6) echo "SET s:1 $v"; echo "GET s:1" ;;
    W3) echo "HSET h:1 f:1 $v"; echo "HGET h:1 f:1" ;;
    W4) echo "ZADD z:1 1 m1"; echo "ZRANGEBYSCORE z:1 1 +inf LIMIT 0 10" ;;
    W5) echo "LPUSH l:1 $v"; echo "LPOP l:1" ;;
    *) echo "wl_probe: unknown workload $1" >&2; return 2 ;;
  esac
  if [[ $1 == W6 ]]; then echo "DEL bh:probe-missing"; fi
}

wl_verify_cmds() {
  echo "$((S_KEYS + H_COUNT + Z_COUNT + L_COUNT + BH_COUNT)) DBSIZE"
  echo "$H_FIELDS HLEN h:1"
  echo "$Z_MEMBERS ZCARD z:1"
  echo "$L_ELEMS LLEN l:1"
  echo "$BH_FIELDS HLEN bh:1"
}

wl_logical_bytes() {
  local v=$VALUE_SIZE
  echo $(( S_KEYS * (v + 12) + H_COUNT * H_FIELDS * (v + 10) + Z_COUNT * Z_MEMBERS * 20 \
         + L_COUNT * L_ELEMS * v + BH_COUNT * BH_FIELDS * (v + 6) ))
}
