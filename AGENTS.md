# GKI project instructions

This project runs an ARM64 Android GSI on QEMU. Read `README.md` for layout and
`.ci/README.md` for the reproducible build. Work within the user's requested scope;
repository setup, commits, pushes and publication require an explicit request.

## Source and build boundaries

- Tracked in git: `qemu/`, `thirdparty/`, `prebuilts/` (host + android-arm64),
  `toolchains/`, `src/` including each variant’s runtime GSI metadata and
  `images/` support files (the large `system.img` is downloaded on demand),
  `qemu/vendor`, `prebuilts/gsi-lib64/`, `hardware/`, KeyMint `keybox.xml`,
  `.ci/`, `.ai/`, scripts and docs. Kernel modules (`*.ko`) are tracked in Git;
  `system.img` and `gki/Image` download at runtime from mirror.opencecs.com.
  Do **not** commit rebuildable `images/vendor.img` or `images/initramfs.img`
  (produced by `./run` / `build-vendor-img.sh` / `build-initramfs.sh`).
- Ignored build products only: `out/`, `dist/`, `downloads/` (CI download cache),
  `archives/` (legacy; unused), `src/aosp/*/images/{system,vendor,initramfs}.img`,
  plus local `.env` / editor settings.
- Variant-specific GSI artifacts belong under `src/aosp/<variant>/images/`, not
  under shared `prebuilts/gsi/`. Host QEMU/VirGL and guest Mesa/libdrm installs
  stay in `prebuilts/host/` and `prebuilts/android-arm64/`.
- `src/aosp/<variant>/hardware/` and `qemu/` (vendor + busybox + fixups) are
  maintained project code. Keep intermediate objects out of `out/` only.
- `scripts/env.sh` owns host, SDK/NDK and prebuilt paths. Support macOS ARM64 and
  Linux ARM64 for host QEMU. Android ARM64 cross-compilation uses macOS or Linux
  x86_64 because Google's Linux NDK host tools are x86_64.
- `.ci/guest.lock.json` pins GSI/busybox download URLs and kernel/module digests
  for CI. Prefer editing the tracked trees in-place. Do not delete nested sources
  to “save space” without asking.
- `thirdparty/virglrenderer/`, `thirdparty/audio-deps/`,
  `thirdparty/android-headers/` and `thirdparty/reference/` may all be kept in
  tree when useful; do not strip them for “cleanliness” without asking.
- Preserve existing work. Do not initialize Git in the project or its parent as a
  side effect.
- Before working inside `qemu/`, read its own `AGENTS.md` and provenance policy.
  These local experiments are not prepared as upstream QEMU submissions.

## Graphics and boot invariants

- The confirmed macOS renderer is `virgl (Apple M4)`. Use `FORCE_VIRGL=1` when
  verifying GPU behavior. Software rendering is a diagnostic option, not proof
  that a requested GPU fix works.
- Preserve 1080 × 2400 portrait, 420 dpi and the direct virtio touchscreen.
- Keep Apple SDL at `gl=core` / OpenGL 4.1 and retain the VirGL glyph texture
  fixes. Do not put Homebrew Mesa's `lib` directory on `DYLD_LIBRARY_PATH`.
- GPU buffers use GBM typed images and direct KMS scanout. CPU copies / DIRTYFB
  on that path can overwrite GPU-rendered content.
- Use the software KeyMint in `hardware/keymint/soft`; the old KeyMint stub cannot
  provide the crypto operations Android needs to finish booting.
- Default validation uses `SNAPSHOT=1` and read-only `system.img`. Keep
  `out/test-<variant>/userdata.img`. Stop a VM before replacing its input images.

## Validation and reporting

Run `python3 .ci/validate.py` for configuration/source-lock changes, then the
relevant builds. A successful compile or `-device help` does not establish that
Android booted or text rendered. For a real guest, check boot completion,
renderer, display size/density and crash logs using `scripts/qemu-console.py`.
Report what was actually tested and identify unavailable platforms explicitly.
Default guest launch is `./run` (lunch menu over `src/aosp/*`, auto-rebuilds
stale `vendor.img` / `initramfs.img`, kernel from each variant’s `KERNEL`).

Project skills live in `.ai/skills/` (`gki-build`, `gki-gsi`, `gki-hal`,
`gki-ci`); prompts live in `.ai/prompts/`. Tool aliases are under
`.agents/skills/`, `.claude/skills/` and `.cursor/skills/`. Use only the material
relevant to the current task.
