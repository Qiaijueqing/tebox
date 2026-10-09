#!/usr/bin/env bash
# Build the local VirGL library used by the QEMU launcher.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
BUILD="$ROOT/out/virglrenderer-build"
export PKG_CONFIG_PATH="$EPOXY_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
ARGS=(setup)
[[ ! -f "$BUILD/build.ninja" ]] || ARGS+=(--reconfigure --clearcache)
if [[ "$HOST_OS" == darwin ]]; then
  ARGS+=('-Dplatforms=[]')
else
  ARGS+=('-Dplatforms=egl')
fi
# Hardware video is opt-in. Linux uses VA-API through a DRM render node;
# macOS uses the host VideoToolbox encoder directly from virglrenderer.
VIRGL_VIDEO="${VIRGL_VIDEO:-0}"
VIDEO_ARGS=(-Dtests=false -Dvenus=false)
if [[ "$VIRGL_VIDEO" == 1 ]]; then
  if [[ "$HOST_OS" == linux ]]; then
    command -v pkg-config >/dev/null 2>&1 && \
      pkg-config --exists libva libva-drm || {
        echo "VIRGL_VIDEO=1 requires pkg-config entries for libva and libva-drm" >&2
        exit 1
      }
  fi
  VIDEO_ARGS+=(-Dvideo=true -Dunstable-apis=true)
else
  # Make a previous video-enabled build reproducibly switch back to the
  # normal renderer when the opt-in is omitted.
  VIDEO_ARGS+=(-Dvideo=false -Dunstable-apis=false)
fi
meson "${ARGS[@]}" "$BUILD" "$ROOT/thirdparty/virglrenderer" \
  --pkg-config-path="$PKG_CONFIG_PATH" \
  --prefix="$VIRGL_PREFIX" --libdir=lib --buildtype=release \
  "${VIDEO_ARGS[@]}"
ninja -C "$BUILD" -j"${JOBS:-8}"
meson install -C "$BUILD" --no-rebuild
