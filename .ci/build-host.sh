#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source scripts/env.sh
[[ "$HOST_ARCH" == arm64 ]] || { echo "Expected an ARM64 host, got $HOST_ID" >&2; exit 1; }
bash .ci/build-libepoxy.sh
bash .ci/build-virglrenderer.sh
QEMU_INSTALL=1 bash .ci/build-qemu.sh
# The installed binary is linked to the project libepoxy and VirGL, which
# are not on the system library path. Same search path as scripts/boot-qemu.sh.
if [[ "$HOST_OS" == darwin ]]; then
  export DYLD_LIBRARY_PATH="$EPOXY_PREFIX/lib:$VIRGL_PREFIX/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
else
  export LD_LIBRARY_PATH="$EPOXY_PREFIX/lib:$VIRGL_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
"$HOST_PRE/qemu/bin/qemu-system-aarch64" --version
"$HOST_PRE/qemu/bin/qemu-system-aarch64" -accel help
python3 .ci/package-host.py
