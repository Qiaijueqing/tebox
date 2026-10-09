#!/usr/bin/env bash
# Cross-build guest Mesa (VirGL / virtio_gpu) for aarch64 Android → prebuilt/arm64.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
SRC="$ROOT/thirdparty/mesa/src"
OUT="$ROOT/out/mesa-android"
PRE="$ROOT/thirdparty/mesa/prebuilt/arm64"
# VirGL's guest video frontend is built with the codecs requested here.  The
# host still decides which profiles are actually available through VA-API.
MESA_VIDEO_CODECS="${MESA_VIDEO_CODECS:-h264dec,h264enc,h265dec,h265enc,vp9dec,av1dec}"

# CI exports ANDROID_NDK. A local tree may only have an SDK or Homebrew NDK.
if [[ ! -x "$NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/clang" ]]; then
  found=
  shopt -s nullglob
  for c in \
    "$HOME/Android/Sdk/ndk/"* \
    "$HOME/Library/Android/sdk/ndk/"* \
    /opt/homebrew/Caskroom/android-ndk/*/AndroidNDK*/Contents/NDK \
    /usr/local/lib/android/sdk/ndk/*; do
    if [[ -x "$c/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/clang" ]]; then
      found=$c
      break
    fi
  done
  shopt -u nullglob
  if [[ -z "$found" ]]; then
    echo "set ANDROID_NDK to an NDK with a $NDK_HOST_TAG toolchain" >&2
    exit 1
  fi
  NDK=$found
  export ANDROID_NDK="$NDK"
fi
source "$(dirname "$0")/android-cross.sh"
[[ -d "$SRC/.git" || -f "$SRC/meson.build" ]] || {
  echo "missing mesa src; run .ci/fetch-mesa-src.sh" >&2
  exit 1
}

if [[ -z "${JOBS:-}" ]]; then
  if [[ "$HOST_OS" == darwin ]]; then
    JOBS="$(sysctl -n hw.ncpu)"
  else
    JOBS="$(nproc)"
  fi
fi
mkdir -p "$OUT" "$PRE"/{egl,dri,hw}

BUILD="$OUT/build"
VIDEO_STAMP="$BUILD/.gki-video-codecs"
NEED_RECONF=0
[[ "${MESA_RECONF:-0}" == "1" || ! -f "$BUILD/build.ninja" ]] && NEED_RECONF=1
if [[ -f "$BUILD/build.ninja" && ! -f "$VIDEO_STAMP" ]]; then
  NEED_RECONF=1
elif [[ -f "$VIDEO_STAMP" && "$(cat "$VIDEO_STAMP")" != "$MESA_VIDEO_CODECS" ]]; then
  NEED_RECONF=1
fi
if [[ "$NEED_RECONF" == 1 ]]; then
  rm -rf "$BUILD"
  VIDEO_ARGS=()
  if [[ -n "${MESA_VIDEO_CODECS:-}" ]]; then
    VIDEO_ARGS+=("-Dvideo-codecs=${MESA_VIDEO_CODECS}")
  fi
  meson setup "$BUILD" "$SRC" \
    --cross-file "$CROSS" \
    --prefix="$OUT/install" \
    --libdir=lib \
    --buildtype=release \
    -Dplatforms=android \
    -Dplatform-sdk-version="$SDK_VER" \
    -Dandroid-stub=true \
    -Dandroid-libbacktrace=disabled \
    -Degl=enabled \
    -Dgles1=enabled \
    -Dgles2=enabled \
    -Dglx=disabled \
    -Dgbm=enabled \
    -Dllvm=disabled \
    -Dshared-llvm=disabled \
    -Dcpp_rtti=false \
    -Dlmsensors=disabled \
    -Dgallium-drivers=virgl,softpipe \
    -Dvulkan-drivers= \
    -Dgallium-vdpau=disabled \
    -Dgallium-va=disabled \
    -Dgallium-xa=disabled \
    "${VIDEO_ARGS[@]}" \
    -Dexpat=disabled \
    -Dlibunwind=disabled \
    -Dxmlconfig=disabled
  printf '%s\n' "$MESA_VIDEO_CODECS" > "$VIDEO_STAMP"
fi

ninja -C "$BUILD" -j"$JOBS"
meson install -C "$BUILD" --no-rebuild

# Stage Android EGL loader names + DRI
INST="$OUT/install/lib"
mkdir -p "$PRE/egl" "$PRE/dri"
shopt -s nullglob
# EGL/GLES
for pair in \
  "libEGL.so:libEGL_mesa.so" \
  "libGLESv1_CM.so:libGLESv1_CM_mesa.so" \
  "libGLESv2.so:libGLESv2_mesa.so"; do
  src=${pair%%:*}; dst=${pair##*:}
  if [[ -f "$INST/$src" ]]; then
    cp -f "$INST/$src" "$PRE/egl/$dst"
  elif [[ -f "$INST/$dst" ]]; then
    cp -f "$INST/$dst" "$PRE/egl/$dst"
  fi
done
# glapi / gallium / dri
[[ -f "$INST/libglapi.so" ]] && cp -f "$INST/libglapi.so" "$PRE/"
for f in "$INST"/libgallium*.so "$INST"/dri/*.so "$INST"/*_dri.so; do
  [[ -f "$f" ]] || continue
  cp -f "$f" "$PRE/dri/"
done
# Also copy any remaining .so that look useful
for f in "$INST"/*.so; do
  base=$(basename "$f")
  case "$base" in
    libEGL*|libGLES*|libglapi*|libgbm*|libgallium*) cp -f "$f" "$PRE/" ;;
  esac
done
shopt -u nullglob

echo "==== staged ===="
find "$PRE" -type f | sort
file "$PRE/egl"/* "$PRE/dri"/* 2>/dev/null | head -20
echo "OK → $PRE (run scripts/build-vendor-img.sh next)"
