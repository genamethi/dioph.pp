#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LUAROCKS_PATH=$1
shift
RUN_DIR="${RUN_DIR:-$HERE/../native/build/lua}"
PRESET_CONFIG="${PRESET_CONFIG:-$HERE/lua/luarocks.preset.lua}"
ROCKS_MANIFEST="${ROCKS_MANIFEST:-$HERE/lua/rocks.lua}"
LUA_VERSION="${LUA_VERSION:-5.5}"

mkdir -p "$RUN_DIR/etc" "$RUN_DIR/rocks"
RUN_DIR="$(cd "$RUN_DIR" && pwd)"
ROCKS_TREE="$RUN_DIR/rocks"
TARGET_CONFIG="$RUN_DIR/etc/luarocks.lua"

STAGE_PREFIX="$(cd "$LUAROCKS_PATH/.." && pwd)"

cp "$PRESET_CONFIG" "$TARGET_CONFIG"
cat >>"$TARGET_CONFIG" <<EOF
variables.LUA_BINDIR = "$STAGE_PREFIX/bin"
variables.LUA_INCDIR = "$STAGE_PREFIX/include"
variables.LUA_LIBDIR = "$STAGE_PREFIX/lib"
EOF
export LUAROCKS_CONFIG="$TARGET_CONFIG"

export LUA_PATH="$STAGE_PREFIX/share/lua/$LUA_VERSION/?.lua;$STAGE_PREFIX/share/lua/$LUA_VERSION/?/init.lua;${LUA_PATH:-;}"
export LUA_CPATH="$STAGE_PREFIX/lib/lua/$LUA_VERSION/?.so;${LUA_CPATH:-;}"

rocks() { "$LUAROCKS_PATH"/luarocks --lua-version="$LUA_VERSION" --tree="$ROCKS_TREE" "$@"; }

if [ $# -gt 0 ]; then
  exec "$LUAROCKS_PATH"/luarocks --lua-version="$LUA_VERSION" --tree="$ROCKS_TREE" "$@"
fi

while read -r name version; do
  [ -n "$name" ] || continue
  if rocks show "$name" ${version:+"$version"} >/dev/null 2>&1; then
    echo "rock $name ${version:-(any)} present"
  else
    echo "==> installing $name ${version:-(latest)} into $ROCKS_TREE"
    rocks install "$name" ${version:+"$version"}
  fi
done < <(ROCKS_MANIFEST="$ROCKS_MANIFEST" lua -e '
  for _, r in ipairs(dofile(os.getenv("ROCKS_MANIFEST"))) do print(r) end
')
