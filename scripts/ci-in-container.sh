#!/usr/bin/env bash
# Runs the CI jobs inside the docker/ci.Dockerfile image; started by scripts/docker-ci.sh.
# /src is the read-only checkout, logs go to /logs. Builds happen in a copy of the tracked files.
set -u
git config --global --add safe.directory /src
mkdir -p /work
git -C /src ls-files -z | (cd /src && xargs -0 cp --parents -t /work)
cd /work
J=$(nproc)
JOBS=" ${JOBS:-debug release asan tsan coverage format boundary tidy fuzz compat} "
FUZZ_SECONDS=${FUZZ_SECONDS:-120}
DIFF_SEEDS=${DIFF_SEEDS:-1-200}
declare -A R
run() {
  [[ "$JOBS" == *" $1 "* ]] || return 0
  local name=$1
  shift
  echo "=== $name"
  if ("$@") >"/logs/$name.log" 2>&1; then R[$name]=PASS; else R[$name]=FAIL; fi
  echo "$name: ${R[$name]}"
  tail -3 "/logs/$name.log"
}
preset() { cmake --preset "$1" -DTIDESKV_WERROR=ON && cmake --build --preset "$1" -j"$J"; }

for p in debug release asan; do
  run "$p" bash -c "$(declare -f preset); J=$J; preset $p && ctest --preset $p -j$J"
done
run tsan bash -c "$(declare -f preset); J=$J; preset tsan && setarch \$(uname -m) -R ctest --preset tsan -j$J"
run coverage env TIDESKV_CMAKE_ARGS=-DTIDESKV_WERROR=ON scripts/coverage.sh
run format bash -c 'clang-format-18 --dry-run --Werror $(git -C /src ls-files "*.h" "*.cc")'
run boundary scripts/check_engine_boundary.sh
run tidy bash -c "cmake -S . -B build/tidy -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 && cmake --build build/tidy -j$J && \
  run-clang-tidy-18 -p build/tidy -quiet '^/work/src/'"
run fuzz bash -c "cmake --preset fuzz -DTIDESKV_WERROR=ON && cmake --build --preset fuzz -j$J --target resp_parser_fuzz && \
  mkdir -p corpus && build/fuzz/tests/resp_parser_fuzz corpus -max_total_time=$FUZZ_SECONDS -max_len=65536"
if [[ "$JOBS" == *" compat "* && ! -x build/release/src/tideskv-server ]]; then
  JOBS+="compat-build "
  run compat-build bash -c "cmake --preset release -DTIDESKV_WERROR=ON && cmake --build --preset release -j$J --target tideskv-server"
fi
run compat env TIDESKV_REF_ADDR="$REF_ADDR" TIDESKV_DIFF_SEEDS="$DIFF_SEEDS" \
  scripts/compat.sh build/release/src/tideskv-server --diff

echo "=== summary"
rc=0
for k in "${!R[@]}"; do
  echo "$k: ${R[$k]}"
  [[ ${R[$k]} == PASS ]] || rc=1
done
[[ -n "${HOST_UID:-}" ]] && chown -R "$HOST_UID" /logs
exit $rc
