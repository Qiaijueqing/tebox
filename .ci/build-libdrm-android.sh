#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$(dirname "$0")/android-cross.sh"
BUILD="$ROOT/out/libdrm-android-build"
ARGS=(setup)
[[ ! -f "$BUILD/build.ninja" ]] || ARGS+=(--reconfigure --clearcache)
meson "${ARGS[@]}" "$BUILD" "$ROOT/thirdparty/libdrm" \
  --cross-file "$CROSS" --prefix="$DRM_PREFIX" --libdir=lib \
  -Dintel=disabled -Dradeon=disabled -Damdgpu=disabled -Dnouveau=disabled \
  -Dvmwgfx=disabled -Dfreedreno=disabled -Detnaviv=disabled -Dvc4=disabled \
  -Dfreedreno-kgsl=false -Dvalgrind=disabled -Dcairo-tests=disabled \
  -Dman-pages=disabled -Dtests=false -Dinstall-test-programs=false
ninja -C "$BUILD" -j"${JOBS:-8}"
meson install -C "$BUILD" --no-rebuild
cp "$DRM_PREFIX/lib/pkgconfig/libdrm.pc" "$PKGDIR/libdrm.pc"
