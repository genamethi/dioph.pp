#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LUAROCKS_PATH=$1
shift
RUN_DIR="${RUN_DIR:-$HERE/../native/build/lua}"
ROCKS_MANIFEST="${ROCKS_MANIFEST:-$HERE/lua/rocks.lua}"
LUA_VERSION="${LUA_VERSION:-5.5}"

mkdir -p "$RUN_DIR/etc" "$RUN_DIR/rocks"
RUN_DIR="$(cd "$RUN_DIR" && pwd)"
PRESET_CONFIG="${PRESET_CONFIG:-$RUN_DIR/etc/luarocks.preset.lua}"
ROCKS_TREE="$RUN_DIR/rocks"
TARGET_CONFIG="$RUN_DIR/etc/luarocks.lua"
BUILD_CONFIG="$RUN_DIR/etc/luarocks-build.lua"

STAGE_PREFIX="$(cd "$LUAROCKS_PATH/.." && pwd)"

# TARGET_CONFIG is installed alongside the rocks tree and must name only the
# deployed prefix. BUILD_CONFIG additionally points at the staging tree, which
# is where lua lives until install-deps runs; it is never installed.
cp "$PRESET_CONFIG" "$TARGET_CONFIG"
cp "$PRESET_CONFIG" "$BUILD_CONFIG"
cat >>"$BUILD_CONFIG" <<EOF
variables.LUA_BINDIR = "$STAGE_PREFIX/bin"
variables.LUA_INCDIR = "$STAGE_PREFIX/include"
variables.LUA_LIBDIR = "$STAGE_PREFIX/lib"
EOF
export LUAROCKS_CONFIG="$BUILD_CONFIG"

export LUA_PATH="$STAGE_PREFIX/share/lua/$LUA_VERSION/?.lua;$STAGE_PREFIX/share/lua/$LUA_VERSION/?/init.lua;${LUA_PATH:-;}"
export LUA_CPATH="$STAGE_PREFIX/lib/lua/$LUA_VERSION/?.so;${LUA_CPATH:-;}"

# The staged luarocks wrapper's shebang names the deployed $PREFIX interpreter,
# which does not exist until install-deps runs; drive it with the staged one.
LUA_BIN="$STAGE_PREFIX/bin/lua"
rocks() {
  "$LUA_BIN" "$LUAROCKS_PATH/luarocks" --lua-version="$LUA_VERSION" \
    --tree="$ROCKS_TREE" "$@"
}

if [ $# -gt 0 ]; then
  exec "$LUA_BIN" "$LUAROCKS_PATH/luarocks" --lua-version="$LUA_VERSION" \
    --tree="$ROCKS_TREE" "$@"
fi

while read -r name version; do
  [ -n "$name" ] || continue
  if rocks show "$name" ${version:+"$version"} >/dev/null 2>&1; then
    echo "rock $name ${version:-(any)} present"
  else
    echo "==> installing $name ${version:-(latest)} into $ROCKS_TREE"
    rocks install "$name" ${version:+"$version"}
  fi
done < <(ROCKS_MANIFEST="$ROCKS_MANIFEST" "$LUA_BIN" -e '
  for _, r in ipairs(dofile(os.getenv("ROCKS_MANIFEST"))) do print(r) end
')
