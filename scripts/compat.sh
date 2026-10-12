#!/usr/bin/env bash
# Runs the Go compatibility suites (tests/compat) against a tideskv-server binary.
#   scripts/compat.sh <path/to/tideskv-server> [--diff]
# --diff also runs the differential test against Redis 7.2: uses $TIDESKV_REF_ADDR if set,
# otherwise starts Docker ${TIDESKV_REF_IMAGE:-redis:7.2.5} on 127.0.0.1:${TIDESKV_REF_PORT:-16379}.
set -euo pipefail
BIN=$(realpath "$1")
DIFF=0
[[ "${2:-}" == "--diff" ]] && DIFF=1
cd "$(dirname "$0")/../tests/compat"
CID=""
cleanup() { if [[ -n "$CID" ]]; then docker rm -f "$CID" >/dev/null; fi; }
trap cleanup EXIT
if [[ $DIFF == 1 && -z "${TIDESKV_REF_ADDR:-}" ]]; then
  PORT=${TIDESKV_REF_PORT:-16379}
  CID=$(docker run -d --rm -p "127.0.0.1:$PORT:6379" "${TIDESKV_REF_IMAGE:-redis:7.2.5}" --save "" --appendonly no)
  for _ in $(seq 50); do docker exec "$CID" redis-cli ping >/dev/null 2>&1 && break; sleep 0.2; done
  export TIDESKV_REF_ADDR="127.0.0.1:$PORT"
fi
[[ $DIFF == 0 ]] && unset TIDESKV_REF_ADDR
go test ./... -count=1 -timeout 30m -binPath="$BIN"
