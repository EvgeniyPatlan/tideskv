#!/usr/bin/env bash
# Runs the CI jobs from .github/workflows/ci.yml in a clean Ubuntu 24.04 container, plus the
# differential test against a redis:7.2.5 container.
#   scripts/docker-ci.sh [job ...]
# Jobs: debug release asan tsan coverage format boundary tidy fuzz compat (default: all).
# Env: LOG_DIR (default build/docker-ci-logs), FUZZ_SECONDS (120), DIFF_SEEDS (1-200).
# Only the containers and network this script creates are touched; they are removed on exit.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
IMAGE=tideskv-ci:24.04
TAG=tideskv-ci-$$
LOG_DIR=$(realpath -m "${LOG_DIR:-build/docker-ci-logs}")
mkdir -p "$LOG_DIR"

cleanup() {
  docker rm -f "$TAG-build" "$TAG-redis" >/dev/null 2>&1 || true
  docker network rm "$TAG-net" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker build -q -t "$IMAGE" -f docker/ci.Dockerfile docker >/dev/null
docker network create "$TAG-net" >/dev/null
docker run -d --rm --name "$TAG-redis" --network "$TAG-net" redis:7.2.5 --save "" --appendonly no >/dev/null

# seccomp=unconfined lets TSan's ctest run under setarch -R (ASLR off) inside the container.
docker run --rm --name "$TAG-build" --network "$TAG-net" --security-opt seccomp=unconfined \
  -e JOBS="$*" -e HOST_UID="$(id -u):$(id -g)" -e REF_ADDR="$TAG-redis:6379" \
  -e FUZZ_SECONDS="${FUZZ_SECONDS:-120}" -e DIFF_SEEDS="${DIFF_SEEDS:-1-200}" \
  -v "$ROOT:/src:ro" -v "$LOG_DIR:/logs" \
  "$IMAGE" bash /src/scripts/ci-in-container.sh
