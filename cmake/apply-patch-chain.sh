#!/bin/sh
# Apply an ordered chain of patches to the current directory, skipping the ones already applied.
#
#   apply-patch-chain.sh first.patch second.patch ...
#
# A tree holds some prefix of the chain: none of it, what an earlier configure applied, or all of
# it. That prefix ends at the last patch that reverses cleanly, so the chain is searched from its
# end, and every patch after that one is applied in order. A patch that then fails to apply fails
# the configure.
set -eu

applied=$#
while [ "$applied" -gt 0 ]; do
  eval "patch=\${$applied}"
  if git apply --reverse --check "$patch" 2>/dev/null; then
    break
  fi
  applied=$((applied - 1))
done

i=0
for patch in "$@"; do
  i=$((i + 1))
  [ "$i" -le "$applied" ] && continue
  echo "applying $(basename "$patch")"
  git apply "$patch"
done
