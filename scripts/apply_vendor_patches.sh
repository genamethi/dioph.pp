#!/usr/bin/env bash
# Apply primeparts local patches to the vendored iceberg-cpp submodule.
#
# Idempotent: a sentinel inside the submodule records that patches are applied,
# keyed by the submodule's current HEAD. Re-running is a no-op until the HEAD
# moves (a tag bump), at which point patches are re-applied against the new
# tree.
#
# Bumping iceberg-cpp: check out the new tag in native/vendor/iceberg-cpp, then
# re-run this script. A patch whose `git apply --check` fails has either been
# upstreamed (retire it: delete the .patch and note it in PATCHES.md) or needs a
# forward-port. Each patch's rationale + RETIRE WHEN condition is in its header.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
VENDOR="$REPO_ROOT/native/vendor/iceberg-cpp"
PATCH_DIR="$REPO_ROOT/native/vendor/patches"
SENTINEL="$VENDOR/.primeparts-patched"

if [ ! -e "$VENDOR/.git" ]; then
    echo "error: iceberg-cpp submodule not initialized at $VENDOR" >&2
    echo "       run: git submodule update --init --checkout native/vendor/iceberg-cpp" >&2
    exit 1
fi

HEAD_SHA=$(git -C "$VENDOR" rev-parse HEAD)

if [ -f "$SENTINEL" ] && [ "$(cat "$SENTINEL" 2>/dev/null)" = "$HEAD_SHA" ]; then
    echo "vendor patches already applied at $HEAD_SHA (sentinel up to date)"
    exit 0
fi

shopt -s nullglob
patches=("$PATCH_DIR"/*.patch)
shopt -u nullglob
if [ ${#patches[@]} -eq 0 ]; then
    echo "no patches in $PATCH_DIR — nothing to apply"
    printf '%s' "$HEAD_SHA" > "$SENTINEL"
    exit 0
fi

# Dry-run the whole series first so a drifted tree fails before we touch files.
for p in "${patches[@]}"; do
    if ! git -C "$VENDOR" apply --check "$p" 2>/dev/null; then
        # Already applied? Then the reverse check succeeds — tolerate that.
        if git -C "$VENDOR" apply --reverse --check "$p" 2>/dev/null; then
            continue
        fi
        echo "error: $(basename "$p") does not apply against iceberg-cpp @ $HEAD_SHA" >&2
        echo "       it was likely upstreamed (retire it) or needs a forward-port." >&2
        echo "       inspect: git -C native/vendor/iceberg-cpp apply --verbose '$p'" >&2
        exit 1
    fi
done

for p in "${patches[@]}"; do
    if git -C "$VENDOR" apply --reverse --check "$p" 2>/dev/null; then
        echo "skip (already applied): $(basename "$p")"
        continue
    fi
    git -C "$VENDOR" apply "$p"
    echo "applied: $(basename "$p")"
done

printf '%s' "$HEAD_SHA" > "$SENTINEL"
echo "vendor patches applied at $HEAD_SHA"
