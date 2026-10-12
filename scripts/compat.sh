#!/usr/bin/env bash
# Runs the Go compatibility suites (tests/compat) against a tideskv-server binary.
#   scripts/compat.sh <path/to/tideskv-server> [--diff]
# --diff also runs the differential test against Redis 7.2: uses $TIDESKV_REF_ADDR if set,
# otherwise starts Docker ${TIDESKV_REF_IMAGE:-redis:7.2.5} on 127.0.0.1:${TIDESKV_REF_PORT:-16379}.
set -euo pipefail
usage() { echo "usage: $0 <path/to/tideskv-server> [--diff]" >&2; exit 2; }
[[ $# -ge 1 && $# -le 2 ]] || usage
DIFF=0
if [[ $# == 2 ]]; then
  [[ "$2" == "--diff" ]] || usage
  DIFF=1
fi
BIN=$(realpath -e "$1") || { echo "compat.sh: binary not found: $1" >&2; exit 2; }
[[ -f "$BIN" && -x "$BIN" ]] || { echo "compat.sh: not an executable file: $BIN" >&2; exit 2; }
export GOTOOLCHAIN=local
cd "$(dirname "$0")/../tests/compat"
CID=""
cleanup() { if [[ -n "$CID" ]]; then docker rm -f "$CID" >/dev/null; fi; }
trap cleanup EXIT
if [[ $DIFF == 1 && -z "${TIDESKV_REF_ADDR:-}" ]]; then
  PORT=${TIDESKV_REF_PORT:-16379}
  CID=$(docker run -d --rm -p "127.0.0.1:$PORT:6379" "${TIDESKV_REF_IMAGE:-redis:7.2.5}" --save "" --appendonly no)
  ready=0
  for _ in $(seq 50); do
    if docker exec "$CID" redis-cli ping >/dev/null 2>&1; then ready=1; break; fi
    sleep 0.2
  done
  if [[ $ready == 0 ]]; then
    echo "compat.sh: reference Redis container $CID did not answer PING within 10s" >&2
    exit 1
  fi
  export TIDESKV_REF_ADDR="127.0.0.1:$PORT"
fi
if [[ $DIFF == 1 ]]; then
  export TIDESKV_DIFF_REQUIRED=1 # TestDifferential fails instead of skipping without a reference
else
  unset TIDESKV_REF_ADDR TIDESKV_DIFF_REQUIRED
fi
go test ./... -count=1 -timeout 30m -binPath="$BIN"
