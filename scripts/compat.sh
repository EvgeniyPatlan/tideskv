#!/usr/bin/env bash
# Runs the Go compatibility suites (tests/compat) against a tideskv-server binary.
#   scripts/compat.sh <path/to/tideskv-server> [--diff] [--kvrocks]
# --diff also runs the differential test against Redis 7.2 (started with --enable-debug-command yes, for the
# DEBUG DIGEST that ends every stream): uses $TIDESKV_REF_ADDR if set,
# otherwise starts Docker ${TIDESKV_REF_IMAGE:-redis:7.2.5} on 127.0.0.1:${TIDESKV_REF_PORT:-16379}.
# --kvrocks runs TestKvrocksCases (the C1-C8 common-case streams) against Kvrocks 2.17: uses
# $TIDESKV_KVROCKS_ADDR if set, otherwise starts Docker apache/kvrocks:2.17.0 on
# 127.0.0.1:${TIDESKV_KVROCKS_PORT:-16666}. Alone, it runs only that test; with --diff, the whole suite.
set -euo pipefail
usage() { echo "usage: $0 <path/to/tideskv-server> [--diff] [--kvrocks]" >&2; exit 2; }
[[ $# -ge 1 && $# -le 3 ]] || usage
DIFF=0
KVROCKS=0
for flag in "${@:2}"; do
  case "$flag" in
    --diff) DIFF=1 ;;
    --kvrocks) KVROCKS=1 ;;
    *) usage ;;
  esac
done
BIN=$(realpath -e "$1") || { echo "compat.sh: binary not found: $1" >&2; exit 2; }
[[ -f "$BIN" && -x "$BIN" ]] || { echo "compat.sh: not an executable file: $BIN" >&2; exit 2; }
export GOTOOLCHAIN=local
cd "$(dirname "$0")/../tests/compat"
CIDS=()
cleanup() { if [[ ${#CIDS[@]} -gt 0 ]]; then docker rm -f "${CIDS[@]}" >/dev/null; fi; }
trap cleanup EXIT
# wait_ping <container> <what> <redis-cli args...>: waits up to 10s for PING.
wait_ping() {
  local cid=$1 what=$2
  shift 2
  for _ in $(seq 50); do
    if docker exec "$cid" redis-cli "$@" ping >/dev/null 2>&1; then return 0; fi
    sleep 0.2
  done
  echo "compat.sh: $what container $cid did not answer PING within 10s" >&2
  exit 1
}
if [[ $DIFF == 1 && -z "${TIDESKV_REF_ADDR:-}" ]]; then
  PORT=${TIDESKV_REF_PORT:-16379}
  CID=$(docker run -d --rm -p "127.0.0.1:$PORT:6379" "${TIDESKV_REF_IMAGE:-redis:7.2.5}" --save "" --appendonly no --enable-debug-command yes)
  CIDS+=("$CID")
  wait_ping "$CID" "reference Redis"
  export TIDESKV_REF_ADDR="127.0.0.1:$PORT"
fi
if [[ $KVROCKS == 1 && -z "${TIDESKV_KVROCKS_ADDR:-}" ]]; then
  KPORT=${TIDESKV_KVROCKS_PORT:-16666}
  CID=$(docker run -d --rm -p "127.0.0.1:$KPORT:6666" apache/kvrocks:2.17.0)
  CIDS+=("$CID")
  wait_ping "$CID" "Kvrocks" -p 6666
  export TIDESKV_KVROCKS_ADDR="127.0.0.1:$KPORT"
fi
[[ $DIFF == 1 ]] || unset TIDESKV_REF_ADDR
[[ $KVROCKS == 1 ]] || unset TIDESKV_KVROCKS_ADDR
if [[ $DIFF == 1 || $KVROCKS == 1 ]]; then
  export TIDESKV_DIFF_REQUIRED=1 # the selected differential tests fail instead of skipping without a reference
else
  unset TIDESKV_DIFF_REQUIRED
fi
if [[ $KVROCKS == 1 && $DIFF == 0 ]]; then
  go test ./diff -run '^TestKvrocksCases$' -count=1 -timeout 30m -binPath="$BIN"
elif [[ $KVROCKS == 0 && $DIFF == 1 ]]; then
  # TIDESKV_DIFF_REQUIRED would make TestKvrocksCases fail without Kvrocks; --diff alone does not run it.
  go test ./... -count=1 -timeout 30m -skip '^TestKvrocksCases$' -binPath="$BIN"
else
  go test ./... -count=1 -timeout 30m -binPath="$BIN"
fi
