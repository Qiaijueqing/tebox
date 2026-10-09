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
SYSTEM="$AOSP/images/system.img"
VENDOR="$AOSP/images/vendor.img"
INITRD="$AOSP/images/initramfs.img"
WORKDIR="$ROOT/out/test-$VARIANT"
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
  local candidates=(
    "$HOST_PRE/qemu/bin/qemu-system-aarch64"
    "$HOST_PRE/qemu/qemu"
    "$ROOT/out/qemu-build/qemu-system-aarch64"
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
  export DYLD_LIBRARY_PATH="$EPOXY_PREFIX/lib:$VIRGL_PREFIX/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
else
  export LD_LIBRARY_PATH="$EPOXY_PREFIX/lib:$VIRGL_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

[[ -f "$KERNEL" ]] || { echo "missing $KERNEL (fetch kernel for $KID)" >&2; exit 1; }
[[ -f "$SYSTEM" ]] || { echo "missing $SYSTEM" >&2; exit 1; }
[[ -f "$VENDOR" ]] || { echo "missing $VENDOR" >&2; exit 1; }
[[ -f "$INITRD" ]] || { echo "missing $INITRD" >&2; exit 1; }

if [[ ! -f "$WORKDIR/userdata.img" ]]; then
  dd if=/dev/zero of="$WORKDIR/userdata.img" bs=1048576 count="${USERDATA_MB:-1024}" status=none
  "$E2/mke2fs" -t ext4 -F -L userdata "$WORKDIR/userdata.img"
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
  APPEND+=(androidboot.qemu_adb=1)
  NETDEV+=",hostfwd=tcp:127.0.0.1:$ADB_PORT-:5555"
  echo "adb:     127.0.0.1:$ADB_PORT (guest TCP 5555)"
fi

if [[ "${QEMU_DEBUG:-0}" == 1 ]]; then
  APPEND+=(androidboot.qemu_debug=1 printk.devkmsg=on)
  EXTRA+=(-device virtio-serial-pci
    -chardev socket,id=debug,path="$WORKDIR/debug.sock",server=on,wait=off
    -device virtconsole,chardev=debug)
fi
[[ "${SNAPSHOT:-0}" == 1 ]] && EXTRA+=(-snapshot)
[[ -n "${QMP_SOCKET:-}" ]] && EXTRA+=(-qmp "unix:$QMP_SOCKET,server=on,wait=off")

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
