[English](README.md) | [简体中文](../README.md)

<p align="center">
  <a href="https://opencecs.com">
    <img src="assets/logo.png" alt="OPENCECS" width="128">
  </a>
</p>

<h1 align="center">OPENCECS</h1>

<p align="center">
  <a href="https://opencecs.com"><b>opencecs.com</b></a>
  <a href="https://github.com/opencecs/tebox">GitHub</a>
</p>

<p align="center">
  <b>tebox — a desktop Android GSI runtime base</b><br>
  Generic GSI boot  VirGL acceleration  Soft vendor HAL examples
</p>

<p align="center">
  <a href="https://github.com/opencecs/tebox/stargazers"><img src="https://img.shields.io/github/stars/opencecs/tebox?style=flat-square" alt="stars"></a>
  <a href="https://github.com/opencecs/tebox/network/members"><img src="https://img.shields.io/github/forks/opencecs/tebox?style=flat-square" alt="forks"></a>
  <a href="../LICENSE"><img src="https://img.shields.io/badge/License-Apache%202.0-blue.svg?style=flat-square" alt="license"></a>
  <a href="https://opencecs.com"><img src="https://img.shields.io/badge/Website-opencecs.com-informational?style=flat-square" alt="website"></a>
</p>

## Overview

**tebox** is a desktop **Android GSI runtime base**. It provides generic ARM64 GSI boot on QEMU, with VirGL / Mesa GPU acceleration, a pinned GKI kernel, and soft vendor HAL examples — so you can validate images, develop HALs, and automate bring-up without a physical device.

Repository: [github.com/opencecs/tebox](https://github.com/opencecs/tebox)  Website: [opencecs.com](https://opencecs.com)

### What the base provides

- **Generic GSI runtime**: ships with an official aosp_arm64 GSI as the default example; you can swap in any compatible ARM64 GSI (e.g. Google or Samsung GSI / Treble images, or a system image extracted from devices such as Pixel or Galaxy)
- **Graphics & input**: VirGL GPU acceleration, virtio touchscreen
- **Codecs**: Gallium video pass-through to the host backend (Linux VA-API / macOS VideoToolbox); the Mesa guest registers `c2.mesa.*` codec components (H.264 / H.265 / VP9 / AV1)
- **Boot-oriented HAL examples**: Graphics, KeyMint (soft), Health, Power, Audio soft / stub implementations — trim or replace them for your product

> Note: whether a given image boots fully depends on its Treble / VINTF requirements. Vendor-private partitions and closed-source services need your own adaptation. tebox is an extensible runtime and HAL framework, not a one-click flash package for a specific phone. If you extract or use third-party / OEM images, firmware, or proprietary components, you are responsible for complying with applicable laws, OEM license terms, and copyright; legal and compliance risk rests with you.

### Related resources

- Community GSI image catalog / downloads: [Mystic GSI Updates on SourceForge](https://sourceforge.net/projects/mystic-gsi-updates/files/)

## Use cases

- Validate and compare GSI images on the desktop
- Develop and debug vendor HALs, graphics stacks, and system services
- Wire CI for automated compile / smoke of GSI + HAL changes

## Supported hosts

| Host | Notes |
| --- | --- |
| **macOS ARM64** (Apple silicon, M1–M4) | Recommended; HVF + VirGL verified |
| **Linux ARM64** | Native build and run (KVM) |
| **Linux x86_64** | Primarily for cross-compiling Android ARM64 guests |
| **Windows x86_64** | Native QEMU/VirGL PE runtime |

The guest is an **aarch64 Android GSI**. A usable GPU and graphical session are recommended.

## Screenshots

<p align="center">
  <img src="screenshots/launched.png" width="240" alt="tebox launched">
</p>

## Clone

Install and initialize Git LFS first (macOS: `brew install git-lfs`; Ubuntu/Debian: `sudo apt-get install git-lfs`):

```bash
git lfs install
git clone https://github.com/opencecs/tebox.git
cd tebox
git lfs pull
```

## Quick start

```bash
bash .ci/install-deps.sh
FORCE_VIRGL=1 SNAPSHOT=1 ./run
```

This boots the built-in AOSP GSI example. Replace `src/aosp/<variant>/images/system.img` to try another GSI.

## License

Released under the [Apache License 2.0](../LICENSE). Third-party sources and binaries remain under their own licenses.

## Star History

<a href="https://www.star-history.com/#opencecs/tebox&Date">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=opencecs/tebox&type=Date&theme=dark" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=opencecs/tebox&type=Date" />
   <img alt="Star History Chart" src="https://api.star-history.com/svg?repos=opencecs/tebox&type=Date" width="100%" />
 </picture>
</a>

## Contributors

Thanks to everyone who has contributed to tebox.

<a href="https://github.com/opencecs/tebox/graphs/contributors">
  <img src="https://stg.contrib.rocks/image?repo=opencecs/tebox" alt="contributors" />
</a>

Made with [contrib.rocks](https://stg.contrib.rocks).

Issues / PRs welcome on [GitHub](https://github.com/opencecs/tebox).
