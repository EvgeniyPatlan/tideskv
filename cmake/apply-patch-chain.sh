#!/bin/sh
# Apply an ordered chain of patches to the current directory, skipping the ones already applied.
#
#   apply-patch-chain.sh [--chain-sha256=HASH] first.patch second.patch ...
#
# A tree holds some prefix of the chain: none of it, what an earlier configure applied, or all of
# it. That prefix ends at the last patch that reverses cleanly, so the chain is searched from its
# end, and every patch after that one is applied in order.
#
# --chain-sha256 is ignored here. The build passes a hash of the patch files' contents so that
# editing a patch in place changes this command and makes the build run it again. A tree patched
# with the old version of an edited patch can't take the new one, and the step fails, asking for
# the source tree to be fetched again.
set -eu

case "${1:-}" in
  --chain-sha256=*) shift ;;
esac

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
  if ! git apply "$patch"; then
    echo "error: the patch chain does not apply to $(pwd) (stopped at $(basename "$patch"))." >&2
    echo "error: the tree most likely holds another version of the chain, from before a patch" >&2
    echo "error: was edited. Delete it and its sub-build, then configure again:" >&2
    echo "error:   rm -rf '$(pwd)' '$(dirname "$(pwd)")/tidesdb-subbuild'" >&2
    exit 1
  fi
done
