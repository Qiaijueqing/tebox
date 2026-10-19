#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/../../../../../.." && pwd)
VARIANT=Hyperos-OS3.0.50.2.W-AB-20260122
source "$ROOT/scripts/env.sh"
HAL=$ROOT/src/aosp/$VARIANT/hardware/graphics
GEN=$ROOT/out/graphics-ndk-gen
OUT=$ROOT/out/graphics-stub
GSI_LIBS=$ROOT/src/aosp/$VARIANT/prebuilts/gsi-lib64
API=34
CC=$NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/aarch64-linux-android${API}-clang++
NM=$NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/llvm-nm
rm -rf "$OUT/obj"
mkdir -p "$OUT/obj" "$OUT/bin"
OBJS=(); i=0
compile_one() {
  local src=$1 obj=$OUT/obj/o$i; i=$((i + 1)); echo "CC $(basename "$src")"
  "$CC" -c "$src" -o "$obj.o" -std=c++20 -fPIC -O2 -DLOG_TAG='"graphics-stub"' -DBINDER_STABILITY_SUPPORT -I"$GEN/include" -I"$HAL/stub" -I"$ANDROID_HEADERS" -I"$MESA_PREFIX/include" -I"$DRM_PREFIX/include" -I"$DRM_PREFIX/include/libdrm" -I"$GSI_LIBS/include" -Wno-unused-parameter -Wno-deprecated-declarations
  OBJS+=("$obj.o")
}
compile_one "$HAL/stub/service_stub.cpp"
while IFS= read -r f; do compile_one "$f"; done < <(find "$GEN/src" -name '*.cpp' | sort)
BIN="$OUT/bin/android.hardware.graphics-service"
"$CC" -o "$BIN" "${OBJS[@]}" -L"$GSI_LIBS" -L"$MESA_PREFIX/lib" -Wl,-rpath,/system/lib64 -static-libstdc++ -lbinder_ndk -llog -lgbm_mesa -Wl,--allow-shlib-undefined
"$NM" "$BIN" | grep -E 'getInterfaceVersion|getInterfaceHash' | head -15 || true
bash "$SCRIPT_DIR/build-mapper.sh"
