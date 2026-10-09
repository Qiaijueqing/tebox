#!/usr/bin/env bash
# Custom EGL-enabled epoxy, including the existing macOS dispatch fixes.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
BUILD="$ROOT/out/libepoxy-build"
ARGS=(setup)
[[ ! -f "$BUILD/build.ninja" ]] || ARGS+=(--reconfigure --clearcache)
if [[ "$HOST_OS" == darwin ]]; then
  MESA_HOST="$(brew --prefix mesa)"
  ARGS+=(-Dglx=no -Dx11=false "-Dc_args=-I$MESA_HOST/include" "-Dcpp_args=-I$MESA_HOST/include")
else
  ARGS+=(-Dglx=yes -Dx11=true)
fi
meson "${ARGS[@]}" "$BUILD" "$ROOT/thirdparty/libepoxy" \
  --prefix="$EPOXY_PREFIX" --libdir=lib \
  -Degl=yes -Dtests=false
ninja -C "$BUILD" -j"${JOBS:-8}"
meson install -C "$BUILD" --no-rebuild
