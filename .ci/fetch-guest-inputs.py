#!/usr/bin/env python3
"""Fetch checksum-pinned Android inputs. Run in a clean CI checkout."""
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parent.parent


def download(spec, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        temp = path.with_suffix(path.suffix + '.part')
        subprocess.run(['curl', '-fL', '--retry', '3', '--connect-timeout', '30',
                        spec['url'], '-o', str(temp)], check=True)
        temp.rename(path)
    with path.open('rb') as stream:
        actual = hashlib.file_digest(stream, 'sha256').hexdigest()
    if actual != spec['sha256']:
        raise SystemExit(f'Checksum mismatch; remove only this bad cached download and retry: {path}')


def main():
    lock = json.loads((ROOT / '.ci/guest.lock.json').read_text())
    subprocess.run([
        'bash', str(ROOT / 'scripts/ensure-guest-downloads.sh'), lock['variant']],
        check=True)
    downloads = ROOT / 'downloads'
    busybox = downloads / 'busybox-1.37.0.tar.bz2'
    download(lock['busybox'], busybox)
    source = ROOT / 'thirdparty/busybox'
    if not source.exists():
        stage = ROOT / 'out/busybox-source'
        stage.mkdir(parents=True, exist_ok=True)
        with tarfile.open(busybox) as archive:
            archive.extractall(stage, filter='data')
        (stage / 'busybox-1.37.0').rename(source)
    for name, expected in lock['kernel_files'].items():
        file = ROOT / 'src/kernel' / lock['kernel_id'] / name
        if hashlib.sha256(file.read_bytes()).hexdigest() != expected:
            raise SystemExit(f'Kernel module differs from the tested version: {file}')


if __name__ == '__main__':
    main()
