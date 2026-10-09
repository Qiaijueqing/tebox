#!/usr/bin/env python3
"""Pack bootable per-variant archives and a vendor/init archive without system.img.

Requires dist/qemu-gki-<host>.tar.gz from .ci/package-host.py. Rebuilds
vendor.img and initramfs.img, then writes:

  dist/tebox-<variant>-<host>.tar.gz
      QEMU plus src/aosp/<variant> and src/kernel/<id>, without system.img.
      ./run boots the only system, or offers the lunch menu when several
      archives have been extracted on top of each other.

  dist/tebox-vendor-init.tar.gz
      Every variant's vendor.img and initramfs.img. No system.img and no QEMU.
"""
from __future__ import annotations

import platform
import subprocess
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
POINTER_MARKER = b'version https://git-lfs.github.com/spec/v1'


def host_id() -> str:
    system = platform.system()
    machine = platform.machine()
    if system == 'Darwin':
        name = 'darwin'
    elif system == 'Linux':
        name = 'linux'
    else:
        raise SystemExit(f'unsupported host OS: {system}')
    if machine not in ('arm64', 'aarch64'):
        raise SystemExit(f'expected an ARM64 host, got {machine}')
    return f'{name}-arm64'


def require_blob(path: Path) -> None:
    if not path.is_file():
        raise SystemExit(f'missing {path}')
    with path.open('rb') as handle:
        if handle.read(len(POINTER_MARKER)) == POINTER_MARKER:
            raise SystemExit(f'{path} is a stale pointer file; fetch the real file first')


def variants() -> list[str]:
    found = []
    aosp = ROOT / 'src/aosp'
    if not aosp.is_dir():
        raise SystemExit(f'missing {aosp}')
    for entry in sorted(aosp.iterdir(), key=lambda item: item.name):
        if not entry.is_dir() or not (entry / 'KERNEL').is_file():
            continue
        if (entry / 'images').is_dir() or (entry / 'qemu').is_dir():
            found.append(entry.name)
    if not found:
        raise SystemExit('no variants under src/aosp/*/ (need KERNEL + images|qemu)')
    return found


def build_images(names: list[str]) -> None:
    for name in names:
        variant = ROOT / 'src/aosp' / name
        kernel_id = (variant / 'KERNEL').read_text().strip()
        kernel = ROOT / 'src/kernel' / kernel_id
        modules = sorted((kernel / 'vendor_modules').glob('*.ko'))
        if not modules:
            raise SystemExit(f'missing kernel modules in {kernel / "vendor_modules"}')
        require_blob(modules[0])
        subprocess.run(['bash', str(ROOT / 'scripts/ensure-guest-downloads.sh'), name],
                       cwd=ROOT, check=True)
        require_blob(kernel / 'gki' / 'Image')
        subprocess.run(['bash', str(ROOT / 'scripts/ensure-runtime-imgs.sh'), name],
                       cwd=ROOT, check=True)
        require_blob(variant / 'images' / 'vendor.img')
        require_blob(variant / 'images' / 'initramfs.img')


def add_file(tar: tarfile.TarFile, source: Path, arcname: str, mode: int | None = None) -> None:
    info = tar.gettarinfo(source, arcname)
    if mode is not None:
        info.mode = mode
    if info.issym() or info.islnk():
        tar.addfile(info)
        return
    with source.open('rb') as handle:
        tar.addfile(info, handle)


def add_tree(tar: tarfile.TarFile, source: Path, arcname: str) -> None:
    tar.add(source, arcname=arcname, recursive=True)


def boot_readme(host: str) -> str:
    return (
        'tebox bootable system\n'
        '\n'
        'Extract this archive and run:\n'
        '  ./run\n'
        '\n'
        'One system under src/aosp/ starts immediately. With several systems,\n'
        './run prints a lunch menu and Enter selects 1. You can also pass a\n'
        'variant name or a menu number. Extract another tebox archive for this\n'
        f'same host ({host}) into this directory to add its system.\n'
        '\n'
        'Layout matches the repository: src/aosp/<variant>/ and src/kernel/<id>/.\n'
        'system.img and gki/Image download from mirror.opencecs.com when missing.\n'
        'The archive includes vendor.img and initramfs.img.\n'
        '\n'
        'The first boot creates out/test-<variant>/userdata.img and needs mke2fs\n'
        '(macOS: brew install e2fsprogs; Linux: e2fsprogs). macOS uses HVF.\n'
        'Linux uses /dev/kvm when it is writable, otherwise TCG.\n'
        'A display is required for the VirGL window.\n'
    )


def images_readme() -> str:
    return (
        'vendor.img and initramfs.img only\n'
        '\n'
        'Each system is at src/aosp/<variant>/images/. This archive has no\n'
        'system.img and no QEMU. Pair the images with your own system.img, or\n'
        'use the bootable tebox-<variant>-<host>.tar.gz archive.\n'
    )


def qemu_tree(temp: Path, host: str) -> Path:
    archive = ROOT / 'dist' / f'qemu-gki-{host}.tar.gz'
    if not archive.is_file():
        raise SystemExit(f'missing {archive}; run .ci/package-host.py first')
    with tarfile.open(archive, 'r:gz') as tar:
        if hasattr(tarfile, 'data_filter'):
            tar.extractall(temp, filter='data')
        else:
            tar.extractall(temp)
    children = [path for path in temp.iterdir() if path.name != '.DS_Store']
    if len(children) != 1 or not children[0].is_dir():
        raise SystemExit(f'unexpected layout in {archive}')
    tree = children[0]
    readme = tree / 'README.txt'
    if readme.exists():
        readme.unlink()
    launcher = tree / 'qemu'
    if not launcher.is_file():
        raise SystemExit(f'missing qemu launcher in {archive}')
    return tree


def write_text(temp: Path, name: str, text: str, mode: int) -> Path:
    path = temp / name
    path.write_text(text)
    path.chmod(mode)
    return path


def pack_bootable(names: list[str], host: str, qemu: Path) -> list[Path]:
    written = []
    bundle = f'tebox-{host}'
    for name in names:
        variant = ROOT / 'src/aosp' / name
        kernel_id = (variant / 'KERNEL').read_text().strip()
        archive_path = ROOT / 'dist' / f'tebox-{name}-{host}.tar.gz'
        with tempfile.TemporaryDirectory(prefix='boot-readme-', dir=ROOT / 'out') as temp_name:
            temp = Path(temp_name)
            readme = write_text(temp, 'README.txt', boot_readme(host), 0o644)
            with tarfile.open(archive_path, 'w:gz', compresslevel=6) as tar:
                add_tree(tar, qemu, bundle)
                guest = {
                    variant / 'KERNEL': f'{bundle}/src/aosp/{name}/KERNEL',
                    variant / 'images' / 'vendor.img': f'{bundle}/src/aosp/{name}/images/vendor.img',
                    variant / 'images' / 'initramfs.img': f'{bundle}/src/aosp/{name}/images/initramfs.img',
                    variant / 'qemu' / 'cmdline' / 'boot': f'{bundle}/src/aosp/{name}/qemu/cmdline/boot',
                    ROOT / 'src/kernel' / kernel_id / 'vendor_modules':
                        f'{bundle}/src/kernel/{kernel_id}/vendor_modules',
                    ROOT / 'scripts' / 'boot-qemu.sh': f'{bundle}/scripts/boot-qemu.sh',
                    ROOT / 'scripts' / 'env.sh': f'{bundle}/scripts/env.sh',
                    ROOT / 'scripts' / 'ensure-guest-downloads.sh':
                        f'{bundle}/scripts/ensure-guest-downloads.sh',
                    ROOT / 'scripts' / 'ensure-runtime-imgs.sh':
                        f'{bundle}/scripts/ensure-runtime-imgs.sh',
                    ROOT / 'scripts' / 'check-runtime-inputs.py': f'{bundle}/scripts/check-runtime-inputs.py',
                    ROOT / '.ci' / 'guest.lock.json': f'{bundle}/.ci/guest.lock.json',
                }
                for source, arcname in guest.items():
                    if source.is_dir():
                        add_tree(tar, source, arcname)
                    else:
                        require_blob(source)
                        mode = 0o755 if source.name.endswith('.sh') else None
                        add_file(tar, source, arcname, mode)
                add_file(tar, ROOT / 'run', f'{bundle}/run', 0o755)
                add_file(tar, readme, f'{bundle}/README.txt')
        written.append(archive_path)
        print(archive_path)
    return written


def pack_vendor_init(names: list[str]) -> Path:
    archive_path = ROOT / 'dist' / 'tebox-vendor-init.tar.gz'
    bundle = 'tebox-vendor-init'
    with tempfile.TemporaryDirectory(prefix='vendor-init-readme-', dir=ROOT / 'out') as temp_name:
        temp = Path(temp_name)
        readme = write_text(temp, 'README.txt', images_readme(), 0o644)
        with tarfile.open(archive_path, 'w:gz', compresslevel=6) as tar:
            for name in names:
                variant = ROOT / 'src/aosp' / name
                add_file(tar, variant / 'KERNEL', f'{bundle}/src/aosp/{name}/KERNEL')
                add_file(tar, variant / 'images' / 'vendor.img',
                         f'{bundle}/src/aosp/{name}/images/vendor.img')
                add_file(tar, variant / 'images' / 'initramfs.img',
                         f'{bundle}/src/aosp/{name}/images/initramfs.img')
            add_file(tar, readme, f'{bundle}/README.txt')
    print(archive_path)
    return archive_path


def main() -> None:
    host = host_id()
    names = variants()
    (ROOT / 'out').mkdir(exist_ok=True)
    (ROOT / 'dist').mkdir(exist_ok=True)
    build_images(names)
    with tempfile.TemporaryDirectory(prefix='qemu-pack-', dir=ROOT / 'out') as temp_name:
        qemu = qemu_tree(Path(temp_name), host)
        pack_bootable(names, host, qemu)
    pack_vendor_init(names)


if __name__ == '__main__':
    main()
