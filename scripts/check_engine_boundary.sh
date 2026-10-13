#!/usr/bin/env bash
# Fails if any file under src/ other than the TidesDB adapter touches the TidesDB C API,
# or if any file outside src/script/ includes Lua.
set -euo pipefail
allowed='^src/storage/tides_(storage|status)\.cc$'
offenders=$(grep -rlE '#include <db\.h>|tidesdb_[a-z_]+\(' src | grep -vE "$allowed" || true)
if [[ -n "$offenders" ]]; then
  echo "Engine boundary violated. Only src/storage/tides_storage.cc and tides_status.cc may use TidesDB:"
  echo "$offenders"
  exit 1
fi
lua_offenders=$(grep -rlE '#[[:space:]]*include[[:space:]]*[<"](lua|lauxlib|lualib)\.h[>"]|#[[:space:]]*include[[:space:]]*[<"]script/lua_api\.h[>"]' src \
  | grep -vE '^src/script/' || true)
if [[ -n "$lua_offenders" ]]; then
  echo "Lua boundary violated. Only files under src/script/ may include Lua headers or script/lua_api.h:"
  echo "$lua_offenders"
  exit 1
fi
echo "engine boundary OK"
