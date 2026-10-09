#!/usr/bin/env bash
# Configure + build qemu-system-aarch64 with HVF/KVM, SDL, and virgl (virtio-gpu-gl).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
SRC="$ROOT/qemu"
BUILD="$ROOT/out/qemu-build"
QEMU_PREFIX="${QEMU_PREFIX:-$HOST_PRE/qemu}"
if [[ "$(uname -s)" == Darwin ]]; then
  JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
else
  JOBS="${JOBS:-$(nproc)}"
fi

[[ -x "$SRC/configure" ]] || { echo "missing $SRC" >&2; exit 1; }
[[ -f "$EPOXY_PREFIX/include/epoxy/egl.h" ]] || {
  echo "missing epoxy+EGL at $EPOXY_PREFIX (build host libepoxy with EGL first)" >&2
  exit 1
}
[[ -f "$VIRGL_PREFIX/lib/pkgconfig/virglrenderer.pc" ]] || {
  echo "missing virglrenderer at $VIRGL_PREFIX (build host VirGL first)" >&2
  exit 1
}

export PKG_CONFIG_PATH="$EPOXY_PREFIX/lib/pkgconfig:$VIRGL_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
if [[ "$HOST_OS" == darwin ]]; then
  MESA_HOST="$(brew --prefix mesa)"
  export PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$MESA_HOST/lib/pkgconfig"
  # Prefer our epoxy headers over Homebrew's (no egl.h). Do not put Mesa's
  # lib on DYLD_LIBRARY_PATH — it shadows Apple's private libGL.
  export CPATH="$EPOXY_PREFIX/include:$MESA_HOST/include:${CPATH:-}"
  export DYLD_LIBRARY_PATH="$EPOXY_PREFIX/lib:$VIRGL_PREFIX/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
else
  export LD_LIBRARY_PATH="$EPOXY_PREFIX/lib:$VIRGL_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
# virglrenderer exposes the video flag under its unstable API guard.  Keep
# QEMU's declaration in sync when the optional host video backend is enabled.
if [[ "${VIRGL_VIDEO:-0}" == 1 ]]; then
  export CFLAGS="${CFLAGS:-} -DVIRGL_RENDERER_UNSTABLE_APIS=1"
fi
# Keep project venv (meson/python) ahead of Homebrew.
export PATH="${PATH:-/usr/bin:/bin}:/opt/homebrew/bin"

mkdir -p "$BUILD"
cd "$BUILD"

# Reconfigure if OpenGL is still off or install prefix is missing/wrong.
NEED_RECONF=0
if [[ "${VIRGL_VIDEO:-0}" == 1 ]]; then
  # The video API is selected through a preprocessor define supplied by the
  # virglrenderer pkg-config build, so an existing QEMU configure cache may
  # have been created without it.
  NEED_RECONF=1
fi
if [[ ! -f "$BUILD/build.ninja" ]]; then
  NEED_RECONF=1
elif ! grep -qE '#define CONFIG_OPENGL( 1)?$' "$BUILD/config-host.h" 2>/dev/null; then
  NEED_RECONF=1
elif grep -qE '#define CONFIG_GTK( 1)?$' "$BUILD/config-host.h" 2>/dev/null; then
  # Homebrew GTK on macOS is not X11, so ui/gtk-egl.c does not compile.
  NEED_RECONF=1
elif [[ "${QEMU_INSTALL:-0}" == 1 ]] && ! grep -Fq "\"value\": \"$QEMU_PREFIX\"" "$BUILD/meson-info/intro-buildoptions.json" 2>/dev/null; then
  NEED_RECONF=1
elif [[ "$HOST_OS" != darwin ]] && grep -A1 '"name": "vhost_user"' "$BUILD/meson-info/intro-buildoptions.json" 2>/dev/null | grep -q '"value": "disabled"'; then
  # vhost.c calls vhost_user_has_protocol_feature. The stub used when
  # vhost-user is disabled does not define it, so the Linux link fails.
  NEED_RECONF=1
fi

if [[ "$NEED_RECONF" == 1 ]]; then
  echo "configuring QEMU with virgl/opengl..."
  CONFIG_ARGS=(
    --target-list=aarch64-softmmu
    --enable-sdl
    --disable-gtk
    --enable-slirp
    --enable-virglrenderer
    --enable-opengl
    --disable-docs
    --enable-plugins
  )
  if [[ "$HOST_OS" == darwin ]]; then
    # macOS wants a non-PIE binary. On Linux aarch64, -fno-pie plus the
    # default PIE link fails on __stack_chk_guard (qemu-keymap and others).
    # vhost-user is Linux-only; leave it enabled there so vhost.c can link.
    CONFIG_ARGS+=(--enable-hvf --disable-pie --disable-vhost-user)
  else
    CONFIG_ARGS+=(--enable-vhost-user)
  fi
  if [[ "${QEMU_INSTALL:-0}" == 1 ]]; then
    CONFIG_ARGS+=(--prefix="$QEMU_PREFIX")
  fi
  "$SRC/configure" "${CONFIG_ARGS[@]}"
fi

ninja -C "$BUILD" -j"$JOBS"
"$BUILD/qemu-system-aarch64" -device help 2>/dev/null | grep -i 'virtio-gpu' || true
if "$BUILD/qemu-system-aarch64" -device help 2>/dev/null | grep -q 'virtio-gpu-gl'; then
  echo "OK: virtio-gpu-gl available"
else
  echo "WARN: virtio-gpu-gl still missing — check CONFIG_OPENGL in config-host.h" >&2
  grep -nE 'CONFIG_OPENGL|CONFIG_VIRGL' "$BUILD/config-host.h" || true
  exit 1
fi

if [[ "${QEMU_INSTALL:-0}" == 1 ]]; then
  echo "installing QEMU to $QEMU_PREFIX..."
  ninja -C "$BUILD" install
fi
