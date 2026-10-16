#!/usr/bin/env bash
# Runs the CI jobs from .github/workflows/ci.yml in a clean Ubuntu 24.04 container, plus the
# differential test against a redis:7.2.5 container, plus kvrocks-controller 1.3.0 with an etcd sidecar for the
# controller conformance test (M7a D26; started only when the compat job runs).
#   scripts/docker-ci.sh [job ...]
# Jobs: debug release asan tsan coverage format boundary tidy fuzz compat (default: all).
# Env: TIDESKV_CRASH_ROUNDS (8), LOG_DIR (default build/docker-ci-logs), FUZZ_SECONDS (120), DIFF_SEEDS (1-200).
# Only the containers and network this script creates are touched; they are removed on exit.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
IMAGE=tideskv-ci:24.04
TAG=tideskv-ci-$$
LOG_DIR=$(realpath -m "${LOG_DIR:-build/docker-ci-logs}")
mkdir -p "$LOG_DIR"
ETCD_IMAGE=quay.io/coreos/etcd:v3.5.17
CTL_IMAGE=apache/kvrocks-controller:1.3.0
CTL_DIR=$(mktemp -d)
CTL_ADDR=""

cleanup() {
  if [[ -n "$CTL_ADDR" ]]; then # PF5: keep the controller's and etcd's logs for diagnosis
    docker logs "$TAG-etcd" >"$LOG_DIR/etcd.log" 2>&1 || true
    docker logs "$TAG-ctl" >"$LOG_DIR/controller.log" 2>&1 || true
  fi
  docker rm -f "$TAG-build" "$TAG-redis" "$TAG-etcd" "$TAG-ctl" >/dev/null 2>&1 || true
  docker network rm "$TAG-net" >/dev/null 2>&1 || true
  rm -rf "$CTL_DIR"
}
trap cleanup EXIT

# Starts etcd, waits until it is healthy, then the controller (upstream's scripts/docker setup, D26). No --rm:
# the logs must survive a crash until cleanup copies them.
start_controller() {
  docker run -d --name "$TAG-etcd" --network "$TAG-net" \
    -e ETCD_NAME=etcd0 -e ETCD_LISTEN_CLIENT_URLS=http://0.0.0.0:2379 \
    -e ETCD_ADVERTISE_CLIENT_URLS="http://$TAG-etcd:2379" -e ETCD_LISTEN_PEER_URLS=http://0.0.0.0:2380 \
    -e ETCD_INITIAL_ADVERTISE_PEER_URLS="http://$TAG-etcd:2380" -e ETCD_INITIAL_CLUSTER="etcd0=http://$TAG-etcd:2380" \
    -e ETCD_INITIAL_CLUSTER_STATE=new "$ETCD_IMAGE" >/dev/null
  CTL_ADDR="$TAG-ctl:9379"
  local ready=0
  for _ in $(seq 100); do
    if docker exec "$TAG-etcd" etcdctl endpoint health >/dev/null 2>&1; then ready=1; break; fi
    sleep 0.2
  done
  if [[ $ready == 0 ]]; then
    echo "docker-ci.sh: etcd did not become healthy within 20s (see $LOG_DIR/etcd.log)" >&2
    exit 1
  fi
  chmod 755 "$CTL_DIR"
  cat >"$CTL_DIR/config.yaml" <<EOF
addr: "$CTL_ADDR"
storage_type: etcd
etcd:
  addrs:
    - "$TAG-etcd:2379"
controller:
  failover:
    ping_interval_seconds: 1
    max_ping_count: 3
EOF
  chmod 644 "$CTL_DIR/config.yaml"
  docker run -d --name "$TAG-ctl" --network "$TAG-net" \
    -v "$CTL_DIR/config.yaml:/var/lib/kvctl/config.yaml:ro" "$CTL_IMAGE" >/dev/null
  # D26: pinned by tag; record what the tags resolved to
  docker image inspect --format '{{index .RepoTags 0}} {{index .RepoDigests 0}}' redis:7.2.5 "$ETCD_IMAGE" "$CTL_IMAGE" \
    >"$LOG_DIR/images.txt"
}

docker build -q -t "$IMAGE" -f docker/ci.Dockerfile docker >/dev/null
docker network create "$TAG-net" >/dev/null
docker run -d --rm --name "$TAG-redis" --network "$TAG-net" redis:7.2.5 --save "" --appendonly no --enable-debug-command yes >/dev/null
if [[ $# == 0 || " $* " == *" compat "* ]]; then
  start_controller
fi

# seccomp=unconfined lets TSan's ctest run under setarch -R (ASLR off) inside the container.
docker run --rm --name "$TAG-build" --network "$TAG-net" --security-opt seccomp=unconfined \
  -e JOBS="$*" -e HOST_UID="$(id -u):$(id -g)" -e REF_ADDR="$TAG-redis:6379" -e CONTROLLER_ADDR="$CTL_ADDR" \
  -e TIDESKV_CRASH_ROUNDS="${TIDESKV_CRASH_ROUNDS:-8}" \
  -e FUZZ_SECONDS="${FUZZ_SECONDS:-120}" -e DIFF_SEEDS="${DIFF_SEEDS:-1-200}" \
  -v "$ROOT:/src:ro" -v "$LOG_DIR:/logs" \
  "$IMAGE" bash /src/scripts/ci-in-container.sh
