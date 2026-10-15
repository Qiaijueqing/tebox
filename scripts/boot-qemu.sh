#!/usr/bin/env bash
# Boot one AOSP variant on QEMU. Kernel comes from src/aosp/<variant>/KERNEL.
# Usage: boot-qemu.sh <variant>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VARIANT="${1:?usage: boot-qemu.sh <variant>}"
AOSP="$ROOT/src/aosp/$VARIANT"
[[ -d "$AOSP" ]] || { echo "missing $AOSP" >&2; exit 1; }
[[ -f "$AOSP/KERNEL" ]] || { echo "missing $AOSP/KERNEL" >&2; exit 1; }

KID="$(tr -d '[:space:]' < "$AOSP/KERNEL")"
K="$ROOT/src/kernel/$KID"
KERNEL="$K/gki/Image"
# SYSTEM_IMG / VENDOR_IMG / INITRD_IMG point an instance at its own images
# (tebox-api CreateInstance); the variant's images are the default.
SYSTEM="${SYSTEM_IMG:-$AOSP/images/system.img}"
VENDOR="${VENDOR_IMG:-$AOSP/images/vendor.img}"
INITRD="${INITRD_IMG:-$AOSP/images/initramfs.img}"
WORKDIR="$ROOT/out/test-$VARIANT"
# The service client gives every Android instance its own userdata, QMP and
# debug sockets.  Keep the legacy per-variant directory when no instance was
# requested from the command line.
if [[ -n "${TEBOX_INSTANCE_DIR:-}" ]]; then
  WORKDIR="${TEBOX_INSTANCE_DIR%/}"
fi
python3 "$ROOT/scripts/check-runtime-inputs.py" "$KERNEL" "$SYSTEM" "$VENDOR" "$INITRD"
mkdir -p "$WORKDIR"

source "$ROOT/scripts/env.sh"

resolve_qemu() {
  if [[ -n "${QEMU:-}" ]]; then
    if [[ -x "$QEMU" ]]; then
      echo "$QEMU"
    elif command -v "$QEMU" >/dev/null 2>&1; then
      command -v "$QEMU"
    else
      echo "QEMU is not executable: $QEMU" >&2
      return 1
    fi
    return 0
  fi
  # Prefer the tree-built QEMU so local UI patches (ui/sdl2.c, cast hook)
  # take effect. Fall back to prebuilts when absent.
  local candidates=(
    "$ROOT/out/qemu-build/qemu-system-aarch64"
    "$HOST_PRE/qemu/bin/qemu-system-aarch64"
    "$HOST_PRE/qemu/qemu"
    "$ROOT/prebuilts/host/$HOST_ID/qemu/bin/qemu-system-aarch64"
  )
  local c
  for c in "${candidates[@]}"; do
    [[ -x "$c" ]] && { echo "$c"; return; }
  done
  if command -v qemu-system-aarch64 >/dev/null 2>&1; then
    command -v qemu-system-aarch64
    return
  fi
  echo "missing qemu-system-aarch64 (build host QEMU or set QEMU=...)" >&2
  exit 1
}

QEMU_BIN="$(resolve_qemu)"

# Mesa's libGL must not shadow Apple's private libGL on macOS.
if [[ "$HOST_OS" == darwin ]]; then
  export DYLD_LIBRARY_PATH="$EPOXY_PREFIX/lib:$VIRGL_PREFIX/lib:$RUTABAGA_PREFIX/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
else
  export LD_LIBRARY_PATH="$EPOXY_PREFIX/lib:$VIRGL_PREFIX/lib:$RUTABAGA_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

[[ -f "$KERNEL" ]] || { echo "missing $KERNEL (fetch kernel for $KID)" >&2; exit 1; }
[[ -f "$SYSTEM" ]] || { echo "missing $SYSTEM" >&2; exit 1; }
[[ -f "$VENDOR" ]] || { echo "missing $VENDOR" >&2; exit 1; }
[[ -f "$INITRD" ]] || { echo "missing $INITRD" >&2; exit 1; }

if [[ ! -f "$WORKDIR/userdata.img" ]]; then
  # Sparse: host space grows with guest data, not with the configured size.
  # Vendor ROMs install ~700 MB of apps on first boot; 1 GiB fills up.
  dd if=/dev/zero of="$WORKDIR/userdata.img" bs=1048576 count=0 seek="${USERDATA_MB:-8192}" status=none
  "$E2/mke2fs" -t ext4 -F -L userdata -O quota,project \
    -E quotatype=usrquota:grpquota:prjquota "$WORKDIR/userdata.img"
fi
# vold assigns project IDs to app external dirs (Android/data/<pkg>); without
# ext4 project quota getExternalCacheDir() is null and e.g. Camera cannot save.
# Upgrade images made before this in place (the VM is not running yet).
if ! "$E2/dumpe2fs" -h "$WORKDIR/userdata.img" 2>/dev/null | grep -q "features:.* project"; then
  "$E2/tune2fs" -O quota,project -Q usrquota,grpquota,prjquota "$WORKDIR/userdata.img" >/dev/null
fi

ACCEL=tcg
case "$HOST_OS" in
  darwin) ACCEL=hvf ;;
  linux)  [[ -e /dev/kvm && -r /dev/kvm && -w /dev/kvm ]] && ACCEL=kvm || ACCEL=tcg ;;
esac
CPU=host
[[ "$ACCEL" != tcg ]] || CPU=max

CMDLINE_FILE="${CMDLINE_FILE:-$AOSP/qemu/cmdline/boot}"
[[ -f "$CMDLINE_FILE" ]] || { echo "missing cmdline file: $CMDLINE_FILE" >&2; exit 1; }

APPEND=()
while IFS= read -r line || [[ -n "$line" ]]; do
  # trim
  line="${line#"${line%%[![:space:]]*}"}"
  line="${line%"${line##*[![:space:]]}"}"
  [[ -z "$line" || "$line" == \#* ]] && continue
  # allow multiple tokens on one line
  # shellcheck disable=SC2206
  APPEND+=($line)
done < "$CMDLINE_FILE"
((${#APPEND[@]} > 0)) || { echo "empty cmdline in $CMDLINE_FILE" >&2; exit 1; }

EXTRA=()
EXTRA+=(-initrd "$INITRD")

# TCP ADB is on by default (host loopback only). Set QEMU_ADB=0 to disable.
# Initramfs forces ro.adb.secure=0 when androidboot.qemu_adb=1 (no RSA prompt).
NETDEV=user,id=net0
if [[ "${QEMU_ADB:-1}" == 1 ]]; then
  ADB_PORT="${ADB_PORT:-5555}"
  if ! [[ "$ADB_PORT" =~ ^[0-9]+$ ]] || (( ${#ADB_PORT} > 5 )); then
    echo "ADB_PORT must be an integer in 1..65535" >&2
    exit 1
  fi
  ADB_PORT=$((10#$ADB_PORT))
  (( ADB_PORT >= 1 && ADB_PORT <= 65535 )) || {
    echo "ADB_PORT must be an integer in 1..65535" >&2
    exit 1
  }
  # ADB_BIND=0.0.0.0 exposes adb to the LAN (headless remote hosts).
  ADB_BIND="${ADB_BIND:-127.0.0.1}"
  APPEND+=(androidboot.qemu_adb=1)
  NETDEV+=",hostfwd=tcp:$ADB_BIND:$ADB_PORT-:5555"
  echo "adb:     $ADB_BIND:$ADB_PORT (guest TCP 5555)"
fi
# Display refresh rate (graphics HAL vsync). It caps every app's frame rate,
# benchmarks included; 120 lets the GPU run past 60 fps.
APPEND+=(androidboot.tebox.refresh_hz="${REFRESH_HZ:-120}")
# Extra host-specific kernel/androidboot tokens, e.g. the CIX Mali launcher.
if [[ -n "${QEMU_EXTRA_APPEND:-}" ]]; then
  # shellcheck disable=SC2206
  APPEND+=($QEMU_EXTRA_APPEND)
fi

if [[ "${QEMU_DEBUG:-0}" == 1 ]]; then
  APPEND+=(androidboot.qemu_debug=1 printk.devkmsg=on)
  EXTRA+=(-device virtio-serial-pci
    -chardev socket,id=debug,path="$WORKDIR/debug.sock",server=on,wait=off
    -device virtconsole,chardev=debug)
fi
[[ "${SNAPSHOT:-0}" == 1 ]] && EXTRA+=(-snapshot)
[[ -n "${QMP_SOCKET:-}" ]] && EXTRA+=(-qmp "unix:$QMP_SOCKET,server=on,wait=off")
# A second monitor for streamed cast input, so a long-lived control client
# never holds the lifecycle QMP socket (QEMU serves one client per monitor).
[[ -n "${QMP_INPUT_SOCKET:-}" ]] && EXTRA+=(-qmp "unix:$QMP_INPUT_SOCKET,server=on,wait=off")

GPU_DEV=virtio-gpu-pci
# A normal ./run uses the verified GPU path; headless runs can use software KMS.
FORCE_VIRGL="${FORCE_VIRGL:-${GRAPHIC:-1}}"
if [[ "$FORCE_VIRGL" == 1 ]]; then
  DEVICES="$("$QEMU_BIN" -device help)"
  [[ "$DEVICES" == *virtio-gpu-gl-pci* ]] || {
    echo "QEMU has no virtio-gpu-gl-pci; build the project QEMU or use FORCE_VIRGL=0 for diagnostics" >&2
    exit 1
  }
  GPU_DEV=virtio-gpu-gl-pci
fi

# GPU backend, like KERNEL: src/aosp/<variant>/GPU holds "gfxstream" (or
# "gfx"; also the default without the file) or "virgl". FORCE_RUTABAGA=1/0
# overrides it for one boot.
if [[ -z "${FORCE_RUTABAGA:-}" ]]; then
  GPU_BACKEND="$(tr -d '[:space:]' < "$AOSP/GPU" 2>/dev/null || echo gfxstream)"
  case "${GPU_BACKEND:-gfxstream}" in
    gfxstream|gfx) FORCE_RUTABAGA=1 ;;
    virgl) FORCE_RUTABAGA=0 ;;
    *) echo "$AOSP/GPU: unknown backend '$GPU_BACKEND' (use gfxstream or virgl)" >&2; exit 1 ;;
  esac
fi
if [[ "$FORCE_RUTABAGA" == 1 ]]; then
  DEVICES="$("$QEMU_BIN" -device help)"
  [[ "$DEVICES" == *virtio-gpu-rutabaga-pci* ]] || {
    echo "QEMU has no virtio-gpu-rutabaga-pci; build the project QEMU or use FORCE_RUTABAGA=0 for diagnostics" >&2
    exit 1
  }
  # gfxstream with Vulkan only (GuestVulkanOnly): the host needs no OpenGL
  # (macOS has no EGL, and gfxstream's GLES translator needs ANGLE's shader
  # translator there), and composes with Vulkan. The guest runs GLES on
  # Vulkan through the GSI's ANGLE. Blob memory carries the
  # address-space-graphics transport.
  GPU_DEV="virtio-gpu-rutabaga,gfxstream-vulkan=on,x-gfxstream-composer=on,blob=on,hostmem=${RUTABAGA_HOSTMEM:-512M}"
  # Guest side: GLES through the GSI's ANGLE on the gfxstream Vulkan driver
  # (prebuilts/android-arm64/gfxstream); our graphics HAL allocates virtio-gpu
  # resources with minigbm-layout handles (ro.hardware.gralloc stays minigbm).
  APPEND+=(androidboot.tebox.gpu=gfxstream androidboot.hardwareegl=angle
    androidboot.hardware.vulkan=ranchu androidboot.opengles.version=196610
    androidboot.hardware.gltransport=virtio-gpu-asg)
  # HWUI renders with Vulkan (skiavk; HWUI_RENDERER=skiagl overrides): HWUI on
  # ANGLE's GLES misrenders some layers (CIX Mali: launcher scrim drawn
  # transparent; macOS: ColorOS clock dial). HWUI's Vulkan backend needs two
  # queues, so on macOS (MoltenVK has one) gfxstream adds a virtual one.
  APPEND+=(androidboot.hwui.renderer="${HWUI_RENDERER:-skiavk}")
  if [[ "$HOST_OS" == darwin ]]; then
    export GFXSTREAM_RENDERER_FEATURES="${GFXSTREAM_RENDERER_FEATURES:-VulkanVirtualQueue:enabled}"
  fi
  # Linux (R1P): zero-copy display. gfxstream keeps ColorBuffers linear and
  # exports them as dma-bufs that the GL display (egl-headless) imports, so
  # frames are never read back by the CPU (virtio-gpu-rutabaga.c). Drivers
  # that can import but not export image memory (CIX Mali) get ColorBuffers
  # from the dma-buf heap instead (GFXSTREAM_DMABUF_HEAP, local gfxstream patch).
  if [[ "$HOST_OS" == linux ]]; then
    if [[ -z "${GFXSTREAM_DMABUF_HEAP+x}" && -w /dev/dma_heap/system ]]; then
      export GFXSTREAM_DMABUF_HEAP=/dev/dma_heap/system
    fi
    # Cast: the GPU scales into a dma-buf the VPU encodes in place, and the
    # headless display skips its full-frame readback (nothing consumes it).
    if [[ -z "${TEBOX_CAST_DMABUF_HEAP+x}" && -w /dev/dma_heap/system ]]; then
      export TEBOX_CAST_DMABUF_HEAP=/dev/dma_heap/system
    fi
    export TEBOX_EGL_READBACK="${TEBOX_EGL_READBACK:-0}"
    export GFXSTREAM_LINEAR_COLOR_BUFFERS="${GFXSTREAM_LINEAR_COLOR_BUFFERS:-1}"
    export TEBOX_RUTABAGA_DMABUF="${TEBOX_RUTABAGA_DMABUF:-1}"
  fi
  # macOS: Vulkan through MoltenVK (Homebrew molten-vk) unless overridden.
  if [[ "$HOST_OS" == darwin && -z "${ANDROID_EMU_VK_LOADER_PATH:-}" && -f /opt/homebrew/lib/libMoltenVK.dylib ]]; then
    export ANDROID_EMU_VK_LOADER_PATH=/opt/homebrew/lib/libMoltenVK.dylib
  fi
fi


# Same SDL GL window for both backends. rutabaga scanouts reach it as a
# host-native (x8r8g8b8) shadow (virtio-gpu-rutabaga.c), which the GL
# console can upload.
if [[ "$FORCE_VIRGL" == 1 ]]; then
  NGFX=(-display "${QEMU_DISPLAY:-sdl,gl=core,show-cursor=on}")
elif [[ "${GRAPHIC:-1}" == 1 ]]; then
  if [[ "$HOST_OS" == darwin ]]; then
    NGFX=(-display "${QEMU_DISPLAY:-cocoa,zoom-to-fit=on,zoom-interpolation=on,show-cursor=on,full-grab=off}")
  else
    NGFX=(-display "${QEMU_DISPLAY:-sdl,show-cursor=on}")
  fi
else
  NGFX=(-display none)
fi
# PCI slot 6 matches the guest's sysfs GPU labels.
NGFX+=(-device "$GPU_DEV,addr=0x6,xres=${XRES:-1080},yres=${YRES:-2400}"
  -device virtio-keyboard-pci -device virtio-tablet-pci,touchscreen=on)

# Host cast: QEMU tebox-cast-hook → Unix socket → tebox-cast (see src/service/cast/).
if [[ "${TEBOX_CAST:-0}" == 1 ]]; then
  export TEBOX_CAST=1
  export TEBOX_CAST_SOCK="${TEBOX_CAST_SOCK:-$WORKDIR/cast.sock}"
  export TEBOX_CAST_FPS="${TEBOX_CAST_FPS:-60}"
  export TEBOX_CAST_MAX_W="${TEBOX_CAST_MAX_W:-720}" TEBOX_CAST_MAX_H="${TEBOX_CAST_MAX_H:-1600}"
  echo "cast:    TEBOX_CAST=1 sock=$TEBOX_CAST_SOCK fps=$TEBOX_CAST_FPS max=${TEBOX_CAST_MAX_W}x$TEBOX_CAST_MAX_H"
  # Guest audio, microphone and camera reach tebox-cast through virtio-serial
  # ports; the audio and camera HALs find them by name under
  # /sys/class/virtio-ports. Sockets sit next to cast.sock.
  MEDIA_DIR="$(dirname "$TEBOX_CAST_SOCK")"
  # Fixed high slot: auto-assignment would shift virtio-net into slot 6 (GPU).
  EXTRA+=(-device virtio-serial-pci,id=media,addr=0x10)
  MEDIA_PORTS="audio-out:tebox.audio_out audio-in:tebox.audio_in camera:tebox.camera"
  if [[ "$FORCE_RUTABAGA" == 1 ]]; then
    # gfxstream has no VirGL video: the guest Codec2 encoder sends resource
    # ids over tebox.venc and tebox-cast encodes on the host, reading the
    # frames from QEMU's resource server (res.sock, virtio-gpu-rutabaga.c).
    MEDIA_PORTS+=" venc:tebox.venc"
    export TEBOX_RES_SOCK="$MEDIA_DIR/res.sock"
    APPEND+=(androidboot.tebox.video_encode=1)
    if [[ "$HOST_OS" == linux ]]; then
      # Decoding the other way round: the guest sends the bitstream over
      # tebox.vdec, the host V4L2 decoder decodes and QEMU draws each picture
      # into the guest's output buffer on the GPU (blit.sock).
      MEDIA_PORTS+=" vdec:tebox.vdec"
      export TEBOX_BLIT_SOCK="$MEDIA_DIR/blit.sock"
    fi
  fi
  for port in $MEDIA_PORTS; do
    sock="${port%%:*}"
    rm -f "$MEDIA_DIR/$sock.sock"
    EXTRA+=(-chardev "socket,id=${sock/-/_},path=$MEDIA_DIR/$sock.sock,server=on,wait=off"
      -device "virtserialport,bus=media.0,chardev=${sock/-/_},name=${port#*:}")
  done
fi

echo "variant: $VARIANT"
echo "kernel:  $KERNEL  ($KID)"
echo "system:  $SYSTEM"
echo "vendor:  $VENDOR"
echo "initrd:  $INITRD"
echo "cmdline: $CMDLINE_FILE"
echo "qemu:    $QEMU_BIN"
echo "accel:   $ACCEL"
echo "gpu:     $GPU_DEV"
echo "==> Ctrl+C in this terminal stops QEMU"

# Keep QEMU in the terminal foreground: background stdio can trigger
# SIGTTIN/SIGTTOU and stop the VM before boot. Plain stdio preserves Ctrl+C.
exec "$QEMU_BIN" \
  -machine virt,gic-version=3 \
  -cpu "$CPU" \
  -accel "$ACCEL" \
  -m "${MEM:-4096}" \
  -smp "${SMP:-4}" \
  -kernel "$KERNEL" \
  "${EXTRA[@]}" \
  -append "${APPEND[*]}" \
  -drive if=none,file="$SYSTEM",format=raw,id=system,readonly=on \
  -device virtio-blk-pci,drive=system,serial=system \
  -drive if=none,file="$WORKDIR/userdata.img",format=raw,id=userdata \
  -device virtio-blk-pci,drive=userdata,serial=userdata \
  -drive if=none,file="$VENDOR",format=raw,id=vendor,readonly=on \
  -device virtio-blk-pci,drive=vendor,serial=vendor \
  -device virtio-net-pci,netdev=net0 \
  -netdev "$NETDEV" \
  "${NGFX[@]}" \
  -serial "${QEMU_SERIAL:-stdio}"
