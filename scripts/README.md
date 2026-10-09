# scripts/

Runtime and guest-image scripts used by `./run` and local HAL work.

| Script | Purpose |
| --- | --- |
| `env.sh` | Shared paths (NDK, prebuilts, `HOST_ID`). Sourced by other scripts. |
| `ensure-guest-downloads.sh` | Download `system.img` + GKI `Image` from mirror when missing. |
| `ensure-runtime-imgs.sh` | Rebuild `vendor.img` / `initramfs.img` when inputs are newer. |
| `boot-qemu.sh` | Launch QEMU for one variant. |
| `build-vendor-img.sh` | Pack `qemu/vendor` (+ Mesa) into `images/vendor.img`. |
| `build-initramfs.sh` | Build initramfs (busybox + kernel modules + overlays). |
| `build-hals.sh` | Rebuild and install the standard soft HAL set. |
| `extract-gsi-libs.sh` | Extract binder/NDK link libs from the variant GSI. |
| `check-runtime-inputs.py` | Reject missing or stale pointer files before boot/pack. |
| `qemu-console.py` | Run commands on the opt-in QEMU debug console. |
| `adb-scrcpy.py` | ADB + scrcpy smoke test against a running guest. |

Host/Android compile entry points live under [`.ci/`](../.ci/README.md)
(`build-host.sh`, `build-android.sh`, `build-qemu.sh`, …).
