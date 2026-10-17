#!/usr/bin/env bash
# Build the placeholder Samsung vendor HALs (see service.cpp) and install them
# with their init rc and VINTF fragment into qemu/vendor.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../.." && pwd)
VARIANT=$(basename "$(cd "$HERE/../.." && pwd)")
# shellcheck source=scripts/env.sh
source "$ROOT/scripts/env.sh"
GSI_LIBS="$ROOT/src/aosp/$VARIANT/prebuilts/gsi-lib64"
CC=$NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/aarch64-linux-android34-clang++
VENDOR="$HERE/../../qemu/vendor"
OUT="$ROOT/out/samsung-stubs"
mkdir -p "$OUT" "$VENDOR/bin/hw" "$VENDOR/etc/init" "$VENDOR/etc/vintf/manifest"
"$CC" -std=c++20 -O2 -Wall -I"$ANDROID_HEADERS" \
  "$HERE/service.cpp" -o "$OUT/vendor.samsung.stubs-service" \
  -L"$GSI_LIBS" -lbinder_ndk -llog -static-libstdc++ -Wl,--allow-shlib-undefined
install -m 0755 "$OUT/vendor.samsung.stubs-service" "$VENDOR/bin/hw/"
install -m 0644 "$HERE/init/vendor.samsung.stubs-service.rc" "$VENDOR/etc/init/"
install -m 0644 "$HERE/vintf/vendor.samsung.stubs-service.xml" "$VENDOR/etc/vintf/manifest/"
echo "installed vendor.samsung.stubs-service into $VENDOR"
