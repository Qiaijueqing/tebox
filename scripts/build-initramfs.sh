#!/usr/bin/env bash
# Build AOSP-side initramfs: virtio kos + busybox init + vendor SELinux stub → switch_root Android.
set -euo pipefail
umask 022
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VARIANT="${1:-aosp_arm64-BP4A.251205.006}"
AOSP="$ROOT/src/aosp/$VARIANT"
[[ -d "$AOSP" ]] || { echo "missing $AOSP" >&2; exit 1; }
KID="$(tr -d '[:space:]' < "$AOSP/KERNEL")"
K="$ROOT/src/kernel/$KID"
BB="$AOSP/qemu/busybox"
STUB="$AOSP/qemu/vendor"
OUT_IMG="$AOSP/images/initramfs.img"
STAGE="$ROOT/out/initramfs-build-$VARIANT"

python3 "$ROOT/scripts/check-runtime-inputs.py" "$BB" "$K/vendor_modules"

[[ -x "$BB" ]] || { echo "missing aarch64 busybox at $BB" >&2; exit 1; }
[[ -d "$K/vendor_modules" ]] || { echo "missing $K/vendor_modules" >&2; exit 1; }
[[ -d "$STUB/etc/selinux" ]] || { echo "missing vendor stub $STUB" >&2; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE"/{bin,dev,proc,sys,newroot,modules}
cp -f "$BB" "$STAGE/bin/busybox"
chmod +x "$STAGE/bin/busybox"
for a in sh ls cat echo grep mount umount mkdir mknod insmod lsmod mdev ln sleep awk \
         chmod chown switch_root chroot mv rm cp setsid touch; do
  ln -sf busybox "$STAGE/bin/$a"
done
cp -f "$K/vendor_modules/"*.ko "$STAGE/modules/"
cp -a "$STUB" "$STAGE/vendor"
INIT_DIR="$AOSP/qemu/init"
if [[ -d "$INIT_DIR" ]]; then
  # Guest path cannot be /init — that is the busybox PID 1 script.
  mkdir -p "$STAGE/overlays"
  cp -f "$INIT_DIR/init.rc" "$STAGE/overlays/" 2>/dev/null || true
  cp -f "$INIT_DIR/init.ranchu.rc" "$STAGE/overlays/" 2>/dev/null || true
  cp -f "$INIT_DIR/android.hardware.security.keymint-service.rc" "$STAGE/overlays/" 2>/dev/null || true
  cp -f "$INIT_DIR/keystore2.rc" "$STAGE/overlays/" 2>/dev/null || true
  cp -f "$INIT_DIR/surfaceflinger.rc" "$STAGE/overlays/" 2>/dev/null || true
  cp -f "$INIT_DIR/vndkcorevariant.libraries.txt" "$STAGE/overlays/" 2>/dev/null || true
  # Soft wificond (fixed fake SSIDs) — replaces system binary, no kernel wifi.
  if [[ -x "$INIT_DIR/wificond" ]]; then
    cp -f "$INIT_DIR/wificond" "$STAGE/overlays/wificond"
    chmod 0755 "$STAGE/overlays/wificond"
  fi
fi

cat > "$STAGE/init" <<'INIT'
#!/bin/sh
export PATH=/bin
mount -t proc none /proc
mount -t sysfs none /sys
mount -t tmpfs -o mode=0755 tmpfs /dev
rm -f /dev/null
mknod -m 666 /dev/null c 1 3
mknod -m 666 /dev/zero c 1 5
mknod -m 666 /dev/full c 1 7
mknod -m 666 /dev/random c 1 8
mknod -m 666 /dev/urandom c 1 9
mknod -m 660 /dev/kmsg c 1 11
mknod -m 666 /dev/tty c 5 0
mknod -m 600 /dev/console c 5 1
mknod -m 666 /dev/ptmx c 5 2
mknod -m 660 /dev/loop-control c 10 237 2>/dev/null || true
i=0
while [ $i -lt 64 ]; do
  mknod -m 660 /dev/loop$i b 7 $i 2>/dev/null || true
  i=$((i+1))
done
mkdir -p /dev/block /dev/pts /dev/socket /dev/__properties__ /dev/binderfs
mount -t devpts devpts /dev/pts 2>/dev/null || true
# Mount binderfs only — let Android init create /dev/binder symlinks with correct labels.
mount -t binder binder /dev/binderfs 2>/dev/null || true

load(){ f=/modules/$1; [ -f "$f" ] && insmod "$f" 2>/dev/null || true; }
load virtio_pci_modern_dev.ko
load virtio_pci_legacy_dev.ko
load virtio_pci.ko
load virtio_mmio.ko
load virtio_dma_buf.ko
load drm_dma_helper.ko
load virtio-gpu.ko
load virtio_blk.ko
load failover.ko
load net_failover.ko
load virtio_net.ko
load virtio_console.ko
load virtio_input.ko
load virtio_balloon.ko
load virtio-rng.ko
load system_heap.ko
mdev -s 2>/dev/null || true
for name in vda vdb vdc; do
  if [ ! -e /dev/$name ]; then
    maj=$(awk -v n="$name" '$4==n {print $1}' /proc/partitions)
    min=$(awk -v n="$name" '$4==n {print $2}' /proc/partitions)
    [ -n "$maj" ] && mknod /dev/$name b "$maj" "$min"
  fi
  ln -sf ../$name /dev/block/$name 2>/dev/null || true
done

mkdir -p /newroot
mount -t ext4 -o rw /dev/vda /newroot || exec /bin/sh
[ -e /dev/vdb ] && mount -t ext4 /dev/vdb /newroot/data
# Ensure keystore DB dir exists before early_hal (normally created in post-fs-data).
mkdir -p /newroot/data/misc/keystore
chmod 0700 /newroot/data/misc/keystore 2>/dev/null || true
chown 1017:1017 /newroot/data/misc/keystore 2>/dev/null || true
for d in metadata cache mnt tmp; do
  mkdir -p /newroot/$d
  mount -t tmpfs -o mode=0755 tmpfs /newroot/$d 2>/dev/null || true
done

# Prefer real vendor.img (ext4 on vdc); fall back to tmpfs stub copy.
mkdir -p /newroot/vendor
if [ -e /dev/vdc ] && mount -t ext4 -o ro /dev/vdc /newroot/vendor; then
  echo "[init] mounted vendor.img from /dev/vdc"
  ls /newroot/vendor/bin/hw 2>/dev/null || true
else
  echo "[init] WARN: no vendor.img, using tmpfs stub"
  mount -t tmpfs -o mode=0755 tmpfs /newroot/vendor
  cp -a /vendor/. /newroot/vendor/
fi

# Neutralize KeyMint stubs previously injected into shared_blocks system.img.
if [ -f /overlays/init.rc ]; then
  mount -o bind /overlays/init.rc /newroot/system/etc/init/hw/init.rc && \
    echo "[init] bound clean init.rc"
fi
if [ -f /overlays/init.ranchu.rc ]; then
  touch /newroot/init.ranchu.rc 2>/dev/null || true
  mount -o bind /overlays/init.ranchu.rc /newroot/init.ranchu.rc && \
    echo "[init] bound clean init.ranchu.rc"
fi
if [ -f /overlays/android.hardware.security.keymint-service.rc ]; then
  mount -o bind /overlays/android.hardware.security.keymint-service.rc \
    /newroot/system/etc/init/android.hardware.security.keymint-service.rc && \
    echo "[init] neutralized system keymint rc"
fi
if [ -f /overlays/keystore2.rc ]; then
  mount -o bind /overlays/keystore2.rc /newroot/system/etc/init/keystore2.rc && \
    echo "[init] bound non-critical keystore2.rc"
fi
if [ -f /overlays/surfaceflinger.rc ]; then
  mount -o bind /overlays/surfaceflinger.rc /newroot/system/etc/init/surfaceflinger.rc && \
    echo "[init] bound surfaceflinger.rc (console + Mesa DRI path)"
fi
if [ -f /overlays/vndkcorevariant.libraries.txt ]; then
  touch /newroot/system/etc/vndkcorevariant.libraries.txt 2>/dev/null || true
  mount -o bind /overlays/vndkcorevariant.libraries.txt \
    /newroot/system/etc/vndkcorevariant.libraries.txt && \
    echo "[init] bound vndkcorevariant.libraries.txt"
fi
if [ -x /overlays/wificond ]; then
  touch /newroot/system/bin/wificond 2>/dev/null || true
  mount -o bind /overlays/wificond /newroot/system/bin/wificond && \
    echo "[init] bound soft wificond"
fi

# GSI ships ro.adb.secure=1 (RSA dialog). For loopback TCP ADB, force insecure
# adbd before second-stage init loads /system/build.prop.
if grep -q 'androidboot.qemu_adb=1' /proc/cmdline 2>/dev/null && \
   [ -f /newroot/system/build.prop ]; then
  awk '
    BEGIN { done=0 }
    /^ro\.adb\.secure=/ { print "ro.adb.secure=0"; done=1; next }
    { print }
    END { if (!done) print "ro.adb.secure=0" }
  ' /newroot/system/build.prop > /overlays/system.build.prop
  mount -o bind /overlays/system.build.prop /newroot/system/build.prop && \
    echo "[init] adb auth off (ro.adb.secure=0)"
fi

mkdir -p /newroot/proc /newroot/sys /newroot/dev
mount -o bind /proc /newroot/proc
mount -o bind /sys /newroot/sys
mount -o bind /dev /newroot/dev
mount -o bind /dev/pts /newroot/dev/pts 2>/dev/null || true

mkdir -p /newroot/sys/fs/selinux
mount -t selinuxfs selinuxfs /newroot/sys/fs/selinux || {
  echo "[init] FATAL selinuxfs"; exec /bin/sh
}
echo "[init] exec selinux_setup"
exec switch_root /newroot /system/bin/init selinux_setup
INIT
chmod +x "$STAGE/init"

mkdir -p "$(dirname "$OUT_IMG")"
( cd "$STAGE" && find . | cpio -o -H newc 2>/dev/null | gzip -9 ) > "$OUT_IMG"
ls -lh "$OUT_IMG"
echo "wrote $OUT_IMG"
