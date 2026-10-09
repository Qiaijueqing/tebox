#!/usr/bin/env bash
# Shared Android Meson cross file; source after scripts/env.sh from .ci/build-*.sh.
SDK_VER="${PLATFORM_SDK_VERSION:-34}"
if [[ "$HOST_OS-$HOST_ARCH" == linux-arm64 ]]; then
  echo "The official Linux NDK uses x86_64 host binaries. Build Android ARM64 on a Linux x86_64 or macOS runner." >&2
  exit 1
fi
TC="$NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG"
CLANG="$TC/bin/aarch64-linux-android${SDK_VER}-clang"
CLANGXX="$TC/bin/aarch64-linux-android${SDK_VER}-clang++"
[[ -x "$CLANG" ]] || { echo "missing $CLANG; see toolchains/README.md" >&2; exit 1; }
CROSS="$ROOT/out/mesa-android/android-aarch64.cross"
PKGDIR="$ROOT/out/mesa-android/pkgconfig"
mkdir -p "$PKGDIR"
# Include the API directory: the NDK's zlib is an API-specific stub library.
cat > "$PKGDIR/zlib.pc" <<EOF
prefix=$TC/sysroot/usr
libdir=\${prefix}/lib/aarch64-linux-android/$SDK_VER
includedir=\${prefix}/include
Name: zlib
Description: Android NDK zlib
Version: 1.2.11
Libs: -L\${libdir} -lz
Cflags: -I\${includedir}
EOF
cat > "$CROSS" <<EOF
[binaries]
ar = '$TC/bin/llvm-ar'
c = ['$CLANG', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables']
cpp = ['$CLANGXX', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-static-libstdc++']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$TC/bin/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=$PKGDIR', '$(command -v pkg-config)']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$PKGDIR'
EOF
