#!/usr/bin/env bash
# Fetch Android Mesa sources into thirdparty/mesa/src (shallow).
# Cross-build to aarch64 Android is a later step (.ci/build-mesa-android.sh).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DST="$ROOT/thirdparty/mesa/src"
BRANCH="${MESA_BRANCH:-android-15.0.0_r1}"
# AOSP mesa3d tracks freedesktop + Android patches
URL="${MESA_URL:-https://android.googlesource.com/platform/external/mesa3d}"

mkdir -p "$(dirname "$DST")"
if [[ -d "$DST/.git" ]]; then
  echo "already present: $DST"
  git -C "$DST" remote -v | head -2
  git -C "$DST" log -1 --oneline || true
  exit 0
fi

echo "cloning $URL ($BRANCH) → $DST"
# android.googlesource often needs depth + branch
if ! git clone --depth 1 --branch "$BRANCH" "$URL" "$DST"; then
  echo "branch $BRANCH missing; trying mesa-main / default"
  git clone --depth 1 "$URL" "$DST"
fi
git -C "$DST" log -1 --oneline
echo "next: place/build aarch64 virtio_gpu Mesa into thirdparty/mesa/prebuilt/arm64/"
