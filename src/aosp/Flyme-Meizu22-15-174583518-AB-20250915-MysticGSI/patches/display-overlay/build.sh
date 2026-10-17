#!/usr/bin/env bash
# Build the display overlay: the GSI's FrameworksResCommonQva_Sys.apk (one
# resource) plus rounded_corner_radius 0 and no display cutout, installed as
# qemu/vendor/etc/tebox/display-overlay.apk. init.ranchu.rc bind-mounts it over
# the original at post-fs (system.img untouched). Not an initramfs
# system-overlay: that file shows as "(deleted)" after switch_root and zygote
# refuses to fork system_server with it open.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../.." && pwd)
SDK="$ROOT/toolchains/android-sdk"
BT="$SDK/build-tools/36.1.0"
JAR="$SDK/platforms/android-36/android.jar"
OUT="$ROOT/out/display-overlay-flyme"
DEST="$HERE/../../qemu/vendor/etc/tebox"
rm -rf "$OUT" && mkdir -p "$OUT" "$DEST"
"$BT/aapt2" compile --dir "$HERE/res" -o "$OUT/res.zip"
"$BT/aapt2" link -I "$JAR" --manifest "$HERE/AndroidManifest.xml" \
  --min-sdk-version 35 --target-sdk-version 35 -o "$OUT/unsigned.apk" "$OUT/res.zip"
"$BT/zipalign" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
# Preinstalled overlays need a valid signature, not a particular key.
keytool -genkeypair -keystore "$OUT/key.jks" -storepass tebox123 -keypass tebox123 \
  -alias tebox -keyalg RSA -keysize 2048 -validity 10000 -dname CN=tebox >/dev/null 2>&1
"$BT/apksigner" sign --ks "$OUT/key.jks" --ks-pass pass:tebox123 --ks-key-alias tebox \
  --out "$DEST/display-overlay.apk" "$OUT/aligned.apk"
rm -f "$DEST/display-overlay.apk.idsig"
echo "installed $DEST/display-overlay.apk"
