#!/usr/bin/env bash
# Pack vendor (+ optional Mesa prebuilts) into images/vendor.img.
set -euo pipefail
# Guest libraries must be readable independently of the launcher's host umask.
umask 022
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VARIANT="${1:-aosp_arm64-BP4A.251205.006}"
AOSP="$ROOT/src/aosp/$VARIANT"
STUB="$AOSP/qemu/vendor"
IMG="$AOSP/images/vendor.img"
STAGE="$ROOT/out/vendor-img-stage-$VARIANT"
python3 "$ROOT/scripts/check-runtime-inputs.py" "$AOSP/images/system.img"
source "$ROOT/scripts/env.sh"
# Prefer tracked guest Mesa under prebuilts/; fall back to legacy thirdparty path.
MESA_PRE_CANDIDATES=(
  "${MESA_PRE:-$ANDROID_PRE/mesa}"
  "$ROOT/thirdparty/mesa/prebuilt/arm64"
)
MESA_PRE=
for c in "${MESA_PRE_CANDIDATES[@]}"; do
  if [[ -d "$c" ]] && find "$c" -type f \( -name '*.so' -o -name '*.so.*' \) 2>/dev/null | grep -q .; then
    MESA_PRE="$c"
    break
  fi
done
SIZE_MB="${VENDOR_IMG_MB:-128}"

[[ -d "$STUB" ]] || { echo "missing $STUB" >&2; exit 1; }
[[ -x "$E2/mke2fs" && -x "$E2/debugfs" ]] || { echo "need e2fsprogs" >&2; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -a "$STUB"/. "$STAGE"/

# Inject Mesa guest stack if prebuilts exist.
if [[ -n "$MESA_PRE" ]]; then
  echo "injecting Mesa from $MESA_PRE"
  mkdir -p "$STAGE/lib64/egl" "$STAGE/lib64/dri" "$STAGE/lib64/hw" "$STAGE/lib64/gbm"
  copy_tree() {
    local src=$1 dst=$2
    [[ -d "$src" ]] || return 0
    mkdir -p "$dst"
    find "$src" \( -type f -o -type l \) | while read -r f; do
      base=$(basename "$f")
      cp -fL "$f" "$dst/$base"
    done
  }
  copy_tree "$MESA_PRE/egl" "$STAGE/lib64/egl"
  copy_tree "$MESA_PRE/dri" "$STAGE/lib64/dri"
  copy_tree "$MESA_PRE/hw" "$STAGE/lib64/hw"
  copy_tree "$MESA_PRE/gbm" "$STAGE/lib64/gbm"
  shopt -s nullglob
  for f in "$MESA_PRE"/*.so "$MESA_PRE"/*.so.*; do
    base="$(basename "$f")"
    case "$base" in
      *EGL*|*GLES*) cp -fL "$f" "$STAGE/lib64/egl/" ;;
      *_dri.so|libgallium*)
        cp -fL "$f" "$STAGE/lib64/dri/"
        cp -fL "$f" "$STAGE/lib64/"
        ;;
      gralloc.*|hwcomposer.*|mapper.*) cp -fL "$f" "$STAGE/lib64/hw/" ;;
      *) cp -fL "$f" "$STAGE/lib64/" ;;
    esac
  done
  shopt -u nullglob
  if [[ -f "$STAGE/lib64/dri/libgallium_dri.so" ]]; then
    cp -f "$STAGE/lib64/dri/libgallium_dri.so" "$STAGE/lib64/dri/virtio_gpu_dri.so"
  fi
  # Extra GBM backends from install prefix / local out (optional).
  for GBM_SRC in \
      "${MESA_PREFIX:-}/lib/gbm" \
      "$ROOT/out/mesa-android/install/lib/gbm"; do
    [[ -d "$GBM_SRC" ]] && copy_tree "$GBM_SRC" "$STAGE/lib64/gbm"
  done
  [[ -f "$STAGE/lib64/libgbm_mesa.so" ]] || {
    echo "missing libgbm_mesa.so after Mesa inject from $MESA_PRE" >&2
    exit 1
  }
  SYSTEM_IMG="$AOSP/images/system.img"
  [[ -f "$SYSTEM_IMG" ]] || { echo "missing $SYSTEM_IMG" >&2; exit 1; }
  for lib in libcutils.so libhardware.so libbase.so libc++.so libz.so; do
    "$E2/debugfs" -R "dump /system/lib64/$lib $STAGE/lib64/$lib" "$SYSTEM_IMG" >/dev/null 2>&1
    [[ -s "$STAGE/lib64/$lib" ]] || { echo "failed to extract $lib from GSI" >&2; exit 1; }
  done
  ls -la "$STAGE/lib64" "$STAGE/lib64/egl" "$STAGE/lib64/dri" "$STAGE/lib64/gbm" | head -60
else
  echo "note: no Mesa prebuilts under ${MESA_PRE_CANDIDATES[*]} (props already mesa/virtio)"
fi
# Keep Android's stable mapper lookup and Mesa's explicit hw lookup identical.
# Prefer the maintained HAL template over mapper copies bundled with Mesa.
if [[ -f "$STUB/lib64/hw/mapper.stub.so" ]]; then
  mkdir -p "$STAGE/lib64/hw"
  install -m 0644 "$STUB/lib64/hw/mapper.stub.so" "$STAGE/lib64/hw/mapper.stub.so"
elif [[ -f "$STUB/lib64/mapper.stub.so" ]]; then
  mkdir -p "$STAGE/lib64/hw"
  install -m 0644 "$STUB/lib64/mapper.stub.so" "$STAGE/lib64/hw/mapper.stub.so"
fi
if [[ -f "$STAGE/lib64/hw/mapper.stub.so" ]]; then
  install -m 0644 "$STAGE/lib64/hw/mapper.stub.so" "$STAGE/lib64/mapper.stub.so"
fi

if [[ "${VENDOR_IMG_MB:-}" == "" ]]; then
  SIZE_MB=256
fi

mkdir -p "$(dirname "$IMG")"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1048576 count="$SIZE_MB" status=none
"$E2/mke2fs" -t ext4 -F -L vendor -m 0 "$IMG" >/dev/null

CMD="$(mktemp)"
{
  cd "$STAGE"
  find . -type d | sed 's|^\./||' | grep -v '^$' | while read -r d; do echo "mkdir /$d"; done
  find . -type f | sed 's|^\./||' | while read -r f; do echo "write $STAGE/$f /$f"; done
  find . -mindepth 1 | sed 's|^\./||' | while read -r f; do
    case "$f" in
      etc|etc/*|manifest.xml) context=vendor_configs_file ;;
      lib64|lib64/*) context=same_process_hal_file ;;
      overlay|overlay/*) context=vendor_overlay_file ;;
      *) context=vendor_file ;;
    esac
    echo "ea_set /$f security.selinux u:object_r:$context:s0"
  done
  if [[ -f bin/hw/android.hardware.security.keymint-service ]]; then
    echo "ea_set /bin/hw/android.hardware.security.keymint-service security.selinux u:object_r:hal_keymint_system_exec:s0"
  fi
  if [[ -f bin/hw/android.hardware.graphics-service ]]; then
    echo "ea_set /bin/hw/android.hardware.graphics-service security.selinux u:object_r:hal_keymint_system_exec:s0"
  fi
  if [[ -f bin/hw/android.hardware.health-service ]]; then
    echo "ea_set /bin/hw/android.hardware.health-service security.selinux u:object_r:hal_keymint_system_exec:s0"
  fi
  if [[ -f bin/hw/android.hardware.audio-service ]]; then
    echo "ea_set /bin/hw/android.hardware.audio-service security.selinux u:object_r:hal_keymint_system_exec:s0"
  fi
  if [[ -f bin/hw/android.hardware.power-service ]]; then
    echo "ea_set /bin/hw/android.hardware.power-service security.selinux u:object_r:hal_keymint_system_exec:s0"
  fi
  if [[ -f bin/hw/android.hardware.sensors-service ]]; then
    echo "ea_set /bin/hw/android.hardware.sensors-service security.selinux u:object_r:hal_keymint_system_exec:s0"
  fi
  if [[ -f bin/hw/android.hardware.wifi-service ]]; then
    echo "ea_set /bin/hw/android.hardware.wifi-service security.selinux u:object_r:hal_keymint_system_exec:s0"
  fi
  if [[ -f bin/hw/android.hardware.wifi.supplicant-service ]]; then
    echo "ea_set /bin/hw/android.hardware.wifi.supplicant-service security.selinux u:object_r:hal_keymint_system_exec:s0"
  fi
  if [[ -f bin/qemu-wifi-fake-scan.sh ]]; then
    echo "ea_set /bin/qemu-wifi-fake-scan.sh security.selinux u:object_r:hal_keymint_system_exec:s0"
  fi

  if [[ -f bin/hw/android.hardware.radio-service ]]; then
    echo "ea_set /bin/hw/android.hardware.radio-service security.selinux u:object_r:hal_keymint_system_exec:s0"
  fi
  if [[ -f bin/hw/android.hardware.bluetooth-service ]]; then
    echo "ea_set /bin/hw/android.hardware.bluetooth-service security.selinux u:object_r:hal_keymint_system_exec:s0"
  fi
  if [[ -f bin/hw/android.hardware.biometrics.fingerprint-service ]]; then
    echo "ea_set /bin/hw/android.hardware.biometrics.fingerprint-service security.selinux u:object_r:hal_keymint_system_exec:s0"
  fi
  if [[ -f bin/hw/android.hardware.nfc-service ]]; then
    echo "ea_set /bin/hw/android.hardware.nfc-service security.selinux u:object_r:hal_keymint_system_exec:s0"
  fi
  if [[ -f bin/hw/android.hardware.gnss-service ]]; then
    echo "ea_set /bin/hw/android.hardware.gnss-service security.selinux u:object_r:hal_keymint_system_exec:s0"
  fi
  if [[ -f bin/hw/android.hardware.media.c2-service ]]; then
    echo "ea_set /bin/hw/android.hardware.media.c2-service security.selinux u:object_r:hal_keymint_system_exec:s0"
  fi
  echo "ea_set / security.selinux u:object_r:vendor_file:s0"
} > "$CMD"
"$E2/debugfs" -w -f "$CMD" "$IMG" >/dev/null 2>&1
rm -f "$CMD"

echo "wrote $IMG ($(du -h "$IMG" | awk '{print $1}'))"
"$E2/debugfs" -R 'ls -l /lib64' "$IMG" 2>/dev/null | head -20 || true
"$E2/debugfs" -R 'cat /build.prop' "$IMG" 2>/dev/null || true
