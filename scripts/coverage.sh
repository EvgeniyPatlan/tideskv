#!/usr/bin/env bash
# Builds the coverage preset, runs every test, and fails if src/ line coverage < 80% (spec §10.5).
set -euo pipefail
BUILD=build/coverage
# shellcheck disable=SC2086
cmake --preset coverage ${TIDESKV_CMAKE_ARGS:-} >/dev/null
cmake --build --preset coverage -j
rm -rf "$BUILD/profiles" && mkdir -p "$BUILD/profiles"
LLVM_PROFILE_FILE="$PWD/$BUILD/profiles/%p-%m.profraw" ctest --preset coverage
llvm-profdata-18 merge -sparse "$BUILD"/profiles/*.profraw -o "$BUILD/merged.profdata"

bins=()
for b in tests/unit_tests tests/contract_tests tests/integration_tests src/tideskv-server; do
  [[ -x "$BUILD/$b" ]] && bins+=("$BUILD/$b")
done
args=("${bins[0]}")
for b in "${bins[@]:1}"; do args+=(-object "$b"); done
ignore='(_deps/|/tests/|src/app/main\.cc)'

llvm-cov-18 report "${args[@]}" -instr-profile="$BUILD/merged.profdata" --ignore-filename-regex="$ignore"
llvm-cov-18 export "${args[@]}" -instr-profile="$BUILD/merged.profdata" --ignore-filename-regex="$ignore" \
  -summary-only > "$BUILD/summary.json"
python3 - "$BUILD/summary.json" <<'PY'
import json, sys
pct = json.load(open(sys.argv[1]))["data"][0]["totals"]["lines"]["percent"]
print(f"line coverage: {pct:.2f}% (gate: 80%)")
sys.exit(0 if pct >= 80.0 else 1)
PY
