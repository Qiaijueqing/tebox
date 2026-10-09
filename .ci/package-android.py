#!/usr/bin/env python3
"""Package ARM64 guest runtime without bundling the downloaded system image."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    lock = json.loads((ROOT / '.ci/guest.lock.json').read_text())
    variant = lock['variant']
    runtime = Path(f'src/aosp/{variant}/images')
    subprocess.run(['bash', str(ROOT / 'scripts/ensure-guest-downloads.sh'), variant], check=True)
    files = [runtime / 'vendor.img', runtime / 'initramfs.img',
             Path(f'src/aosp/{variant}/KERNEL'),
             Path(f"src/kernel/{lock['kernel_id']}/vendor_modules"),
             Path('scripts/env.sh'), Path('run'),
             Path('scripts/ensure-guest-downloads.sh'),
             Path('scripts/check-runtime-inputs.py'),
             Path('scripts/qemu-console.py'), Path('.ci/guest.lock.json')]
    # Validate guest ELF architecture and the packaged image filesystem.
    for file in (ROOT / 'out').glob('*-stub/bin/android.hardware.*'):
        info = subprocess.check_output(['file', str(file)], text=True)
        if 'aarch64' not in info:
            raise SystemExit(f'Wrong guest architecture: {info}')
    subprocess.run(['/usr/sbin/e2fsck', '-fn', str(ROOT / runtime / 'vendor.img')], check=True)
    dist = ROOT / 'dist'
    dist.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='android-package-', dir=ROOT / 'out') as temp:
        package = Path(temp) / 'gki-android-arm64'
        checksums = {}
        for relative in files:
            destination = package / relative
            source = ROOT / relative
            if source.is_dir():
                shutil.copytree(source, destination, dirs_exist_ok=True)
                for path in sorted(destination.rglob('*')):
                    if path.is_file():
                        checksums[str(path.relative_to(package))] = (
                            hashlib.sha256(path.read_bytes()).hexdigest())
                continue
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
            checksums[str(relative)] = hashlib.sha256(destination.read_bytes()).hexdigest()
        (package / 'SHA256SUMS.json').write_text(json.dumps(checksums, indent=2) + '\n')
        (package / 'README.txt').write_text(
            'Android ARM64 guest runtime (GKI + vendor HAL/Mesa + initramfs).\n'
            'system.img is not included; ./run downloads it on first boot when missing.\n'
            f"Runtime URL: {lock['system']['url']}\nSHA-256: {lock['system']['sha256']}\n"
            'From this directory, use the matching host package wrapper:\n'
            'QEMU=/absolute/path/to/qemu-gki-OS-arm64/qemu FORCE_VIRGL=1 SNAPSHOT=1 ./run\n'
            'Requires e2fsprogs for userdata creation; Linux requires a graphical session and /dev/kvm access.\n'
            'Build validation is not a real-GPU boot test.\n')
        with tarfile.open(dist / 'gki-android-arm64.tar.gz', 'w:gz') as tar:
            tar.add(package, arcname=package.name)


if __name__ == '__main__':
    main()
