#!/usr/bin/env bash
# Apply primeparts vendor patches to native/vendor/iceberg-rust.
# Idempotent: a sentinel file in the vendor tree records that patches are
# applied. Re-running is a no-op once the sentinel is in place.
#
# Re-vendoring iceberg-rust: delete the sentinel, re-run this script, and
# investigate any rejects. Each patch hunk's rationale and removal-condition
# is documented in the .patch file header.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
VENDOR="$REPO_ROOT/native/vendor/iceberg-rust"
PATCH="$REPO_ROOT/crates/vendor-patches/iceberg-rust.patch"
SENTINEL="$VENDOR/.primeparts-patched"

if [ ! -d "$VENDOR" ]; then
    echo "error: vendor tree not found at $VENDOR" >&2
    exit 1
fi
if [ ! -f "$PATCH" ]; then
    echo "error: patch file not found at $PATCH" >&2
    exit 1
fi

if [ -f "$SENTINEL" ]; then
    echo "vendor patches already applied (sentinel: $SENTINEL)"
    exit 0
fi

cd "$VENDOR"

# Dry-run first so we get a clean error before touching files.
if ! patch -p1 --dry-run --silent < "$PATCH"; then
    echo "error: patch dry-run failed against $VENDOR" >&2
    echo "       likely cause: vendor tree drifted since the patch was written." >&2
    echo "       inspect with: patch -p1 --dry-run --verbose < $PATCH" >&2
    exit 1
fi

patch -p1 --silent < "$PATCH"
date -u +"applied at %Y-%m-%dT%H:%M:%SZ from $PATCH" > "$SENTINEL"
echo "vendor patches applied"
