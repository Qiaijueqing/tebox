#!/usr/bin/env bash
# Ensure vendor.img / initramfs.img exist and are newer than their inputs.
# Usage: ensure-runtime-imgs.sh <variant>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VARIANT="${1:?usage: ensure-runtime-imgs.sh <variant>}"
AOSP="$ROOT/src/aosp/$VARIANT"
[[ -d "$AOSP" ]] || { echo "missing variant: $AOSP" >&2; exit 1; }

IMAGES="$AOSP/images"
QEMU_RT="$AOSP/qemu"
VENDOR_IMG="$IMAGES/vendor.img"
INITRD_IMG="$IMAGES/initramfs.img"
STUB="$QEMU_RT/vendor"
KID="$(tr -d '[:space:]' < "$AOSP/KERNEL")"
K="$ROOT/src/kernel/$KID"

# system.img and gki/Image are runtime inputs; download when absent.
bash "$ROOT/scripts/ensure-guest-downloads.sh" "$VARIANT"

# Stale pointer files must never be packed as modules or boot images.
python3 "$ROOT/scripts/check-runtime-inputs.py" \
  "$IMAGES/system.img" "$K/gki/Image" "$K/vendor_modules" "$QEMU_RT/busybox"

img_stale() {
  local img=$1
  shift
  [[ -f "$img" ]] || return 0
  local p
  for p in "$@"; do
    [[ -e "$p" ]] || continue
    if [[ -d "$p" ]]; then
      if [[ -n "$(find "$p" -type f -newer "$img" -print -quit)" ]]; then
        return 0
      fi
    elif [[ -f "$p" && "$p" -nt "$img" ]]; then
      return 0
    fi
  done
  return 1
}

VENDOR_INPUTS=(
  "$STUB"
  "$ROOT/scripts/build-vendor-img.sh"
  "$IMAGES/system.img"
)
[[ -d "$ROOT/prebuilts/android-arm64/mesa" ]] && VENDOR_INPUTS+=("$ROOT/prebuilts/android-arm64/mesa")
[[ -d "$ROOT/thirdparty/mesa/prebuilt/arm64" ]] && VENDOR_INPUTS+=("$ROOT/thirdparty/mesa/prebuilt/arm64")

INIT_INPUTS=(
  "$QEMU_RT/busybox"
  "$STUB"
  "$QEMU_RT/init"
  "$AOSP/KERNEL"
  "$ROOT/scripts/build-initramfs.sh"
)
[[ -d "$K/vendor_modules" ]] && INIT_INPUTS+=("$K/vendor_modules")

need_vendor=0
need_init=0
if img_stale "$VENDOR_IMG" "${VENDOR_INPUTS[@]}"; then
  need_vendor=1
fi
if img_stale "$INITRD_IMG" "${INIT_INPUTS[@]}"; then
  need_init=1
fi

if (( need_vendor == 0 && need_init == 0 )); then
  echo "runtime images up to date for $VARIANT"
  exit 0
fi

if pgrep -f "qemu-system-aarch64.*$VARIANT" >/dev/null 2>&1 || \
   pgrep -f "qemu-system-aarch64.*test-$VARIANT" >/dev/null 2>&1; then
  echo "stop the running QEMU for $VARIANT before rebuilding images" >&2
  exit 1
fi

if (( need_vendor )); then
  echo "rebuilding vendor.img (missing or inputs newer than image)"
  bash "$ROOT/scripts/build-vendor-img.sh" "$VARIANT"
fi
if (( need_init )); then
  echo "rebuilding initramfs.img (missing or inputs newer than image)"
  bash "$ROOT/scripts/build-initramfs.sh" "$VARIANT"
fi
