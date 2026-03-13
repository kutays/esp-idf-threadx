#!/bin/bash
# Apply threadx submodule patches from the project root.
# Run this after a fresh clone if the submodule is at upstream HEAD.
#
# Usage:  bash patches/threadx/apply.sh
#
# Upstream base these patches were written against:
#   Remote : https://github.com/eclipse-threadx/threadx.git
#   Branch : master
#   Commit : 4b6e8100 (Merge commit from fork)
#
# If upstream has advanced past this commit, run:
#   cd components/threadx/threadx
#   git log 4b6e8100..HEAD --oneline
# to see what changed before applying. Patches may need rebasing.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SUBMODULE="$(cd "$SCRIPT_DIR/../.." && pwd)/components/threadx/threadx"

echo "Applying threadx patches to: $SUBMODULE"
cd "$SUBMODULE"

for patch in "$SCRIPT_DIR"/*.patch; do
    echo "  -> $patch"
    git apply "$patch"
done

echo "Done. Modified files:"
git diff --name-only
