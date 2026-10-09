#!/usr/bin/env bash
# Rebuild and install the current hardware HAL set into qemu/vendor.
set -euo pipefail
export VARIANT="${1:-aosp_arm64-BP4A.251205.006}"
source "$(dirname "$0")/env.sh"
HAL="$ROOT/src/aosp/$VARIANT/hardware"
STUB="$ROOT/src/aosp/$VARIANT/qemu/vendor"
[[ -d "$STUB/etc" ]] || { echo "missing vendor template: $STUB" >&2; exit 1; }
bash "$ROOT/scripts/extract-gsi-libs.sh" "$VARIANT"
for name in graphics health audio power sensors wifi radio bluetooth fingerprint nfc gnss; do
  bash "$HAL/$name/stub/build-stub.sh"
done
bash "$HAL/codec2/soft/build.sh"
bash "$HAL/wifi/supplicant-stub/build-stub.sh"
bash "$HAL/wifi/wificond-stub/build-stub.sh"
bash "$HAL/keymint/soft/build.sh"
mkdir -p "$STUB/bin/hw" "$STUB/lib64/hw"
for name in graphics health; do
  install -m 0755 "$ROOT/out/$name-stub/bin/android.hardware.$name-service" "$STUB/bin/hw/"
done
install -m 0755 "$ROOT/out/graphics-stub/lib64/hw/mapper.stub.so" "$STUB/lib64/hw/"
install -m 0644 "$ROOT/out/graphics-stub/lib64/hw/mapper.stub.so" "$STUB/lib64/mapper.stub.so"
for name in audio power sensors wifi radio bluetooth fingerprint nfc gnss; do
  bash "$HAL/$name/stub/install.sh"
done
bash "$HAL/keymint/soft/install.sh"
bash "$HAL/codec2/soft/install.sh"

echo "HALs installed. Repack vendor/initramfs after stopping QEMU (or ./run)."
