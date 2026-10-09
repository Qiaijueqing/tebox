#!/usr/bin/env bash
# Keep all platform ABI link libraries matched to this variant's system.img.
set -euo pipefail
VARIANT="${1:-aosp_arm64-BP4A.251205.006}"
source "$(dirname "$0")/env.sh"
SYSTEM_IMG="${SYSTEM_IMG:-$ROOT/src/aosp/$VARIANT/images/system.img}"
DEBUGFS="$E2/debugfs"
[[ -s "$SYSTEM_IMG" ]] || { echo "missing $SYSTEM_IMG" >&2; exit 1; }
mkdir -p "$GSI_LIBS"
for lib in libbinder_ndk.so libcutils.so liblog.so libc++.so \
    libkeymint_fake_latest.so libkeymaster_portable.so \
    android.hardware.security.keymint-V4-ndk.so \
    android.hardware.security.secureclock-V1-ndk.so \
    android.hardware.security.sharedsecret-V1-ndk.so; do
  tmp="$(mktemp "$GSI_LIBS/.extract.XXXXXX")"
  "$DEBUGFS" -R "dump /system/lib64/$lib $tmp" "$SYSTEM_IMG" >/dev/null 2>&1
  [[ -s "$tmp" ]] || { echo "failed to extract $lib" >&2; exit 1; }
  mv -f "$tmp" "$GSI_LIBS/$lib"
done
echo "GSI link libraries: $GSI_LIBS"
