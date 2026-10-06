#!/usr/bin/env bash
# Fails if any file under src/ other than the TidesDB adapter touches the TidesDB C API.
set -euo pipefail
allowed='^src/storage/tides_(storage|status)\.cc$'
offenders=$(grep -rlE '#include <db\.h>|tidesdb_[a-z_]+\(' src | grep -vE "$allowed" || true)
if [[ -n "$offenders" ]]; then
  echo "Engine boundary violated. Only src/storage/tides_storage.cc and tides_status.cc may use TidesDB:"
  echo "$offenders"
  exit 1
fi
echo "engine boundary OK"
