#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source scripts/env.sh
[[ "$HOST_ID" == linux-x86_64 ]] || { echo 'Use a Linux x86_64 Android build runner' >&2; exit 1; }
python3 .ci/fetch-guest-inputs.py
BB_BUILD="$ROOT/out/busybox-build"
mkdir -p "$BB_BUILD"
make -C thirdparty/busybox O="$BB_BUILD" defconfig
# linux-libc-dev >= 6.8 dropped CBQ from linux/pkt_sched.h. BusyBox 1.37
# defconfig still builds tc, so networking/tc.c fails with missing TCA_CBQ_*.
# The initramfs does not use tc.
python3 - "$BB_BUILD/.config" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
s = p.read_text().replace('# CONFIG_STATIC is not set', 'CONFIG_STATIC=y')
for name in ('CONFIG_TC', 'CONFIG_FEATURE_TC_INGRESS'):
    s = s.replace(f'{name}=y\n', f'# {name} is not set\n')
p.write_text(s)
PY
make -C thirdparty/busybox O="$BB_BUILD" CROSS_COMPILE=aarch64-linux-gnu- -j"${JOBS:-8}"
install -m 0755 "$BB_BUILD/busybox" "src/aosp/$VARIANT/qemu/busybox"
bash .ci/build-libdrm-android.sh
bash .ci/build-mesa-android.sh
bash scripts/build-hals.sh
bash scripts/build-vendor-img.sh
bash scripts/build-initramfs.sh
python3 .ci/package-android.py
