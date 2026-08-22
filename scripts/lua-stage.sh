#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENDOR=$1
BUILD=$2
STAGE_PREFIX=$3
PP_LUA_PATH=$4
LUA_RELEASE="${LUA_RELEASE:-5.5.1}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

SRC="$VENDOR/lua"
WORK="$BUILD/lua-src"
STAMP="$STAGE_PREFIX/lib/.pp-lua.sha"
PATCH_IN="$HERE/lua/pp-path.patch.in"
PATCH="$BUILD/pp-path.patch"

if [ ! -f "$SRC/src/lua.c" ]; then
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  curl -L -R -o "$tmp/lua.tar.gz" "https://www.lua.org/ftp/lua-$LUA_RELEASE.tar.gz"
  tar zxf "$tmp/lua.tar.gz" -C "$tmp"
  mkdir -p "$SRC"
  cp -a "$tmp/lua-$LUA_RELEASE/." "$SRC/"
fi

mkdir -p "$BUILD"
sed "s|@PP_LUA_PATH@|$PP_LUA_PATH|g" "$PATCH_IN" >"$PATCH"

token="lua-$LUA_RELEASE $(sha256sum <"$PATCH" | cut -d' ' -f1)"
if [ "$(cat "$STAMP" 2>/dev/null || true)" = "$token" ]; then
  echo "lua $LUA_RELEASE staged in $STAGE_PREFIX"
  exit 0
fi

rm -rf "$WORK"
mkdir -p "$WORK"
cp -a "$SRC/." "$WORK/"
patch -d "$WORK" -p1 <"$PATCH"

make -C "$WORK" -j"$JOBS" all
make -C "$WORK" test
make -C "$WORK" install INSTALL_TOP="$STAGE_PREFIX"

mkdir -p "$(dirname "$STAMP")"
echo "$token" >"$STAMP"
echo "lua $LUA_RELEASE built from $SRC and staged to $STAGE_PREFIX"
