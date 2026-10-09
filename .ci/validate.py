#!/usr/bin/env python3
"""Validate guest pins, skills, prompts and CI wiring for a clean checkout."""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ERRORS: list[str] = []


def fail(message: str) -> None:
    ERRORS.append(message)


def require(path: Path, label: str | None = None) -> None:
    if not path.exists():
        fail(f'Missing {label or path.relative_to(ROOT)}')


def validate_guest_lock() -> None:
    lock_path = ROOT / '.ci/guest.lock.json'
    require(lock_path)
    if not lock_path.is_file():
        return
    lock = json.loads(lock_path.read_text())
    for key in ('variant', 'system', 'kernel', 'busybox', 'kernel_id', 'kernel_files',
                'kernel_commit', 'modules_commit', 'ndk', 'build_tools'):
        if key not in lock:
            fail(f'guest.lock.json missing {key}')
    system = lock.get('system', {})
    kernel = lock.get('kernel', {})
    busybox = lock.get('busybox', {})
    for label, blob in (('system', system), ('kernel', kernel), ('busybox', busybox)):
        if not isinstance(blob, dict) or 'url' not in blob or 'sha256' not in blob:
            fail(f'guest.lock.json {label} needs url + sha256')
            continue
        if not re.fullmatch(r'[0-9a-f]{64}', blob['sha256']):
            fail(f'guest.lock.json {label}.sha256 is not 64 hex chars')
    files = lock.get('kernel_files', {})
    if not isinstance(files, dict) or not files:
        fail('guest.lock.json kernel_files must be a non-empty map')
    else:
        for name, digest in files.items():
            if not re.fullmatch(r'[0-9a-f]{64}', digest):
                fail(f'guest.lock.json kernel_files[{name}] digest invalid')
    require(ROOT / f"src/aosp/{lock.get('variant', '')}/KERNEL", 'variant KERNEL pointer')
    require(ROOT / f"src/kernel/{lock.get('kernel_id', '')}/FETCHED_FROM.txt",
            'kernel FETCHED_FROM.txt')


def validate_skills_and_prompts() -> None:
    skills_root = ROOT / '.ai/skills'
    require(skills_root)
    if not skills_root.is_dir():
        return
    skill_names = []
    for skill in sorted(skills_root.iterdir()):
        if not skill.is_dir():
            continue
        skill_names.append(skill.name)
        require(skill / 'SKILL.md', f'{skill.name}/SKILL.md')
        require(skill / 'agents/openai.yaml', f'{skill.name}/agents/openai.yaml')
        skill_md = skill / 'SKILL.md'
        if skill_md.is_file():
            text = skill_md.read_text()
            if 'name:' not in text or 'description:' not in text:
                fail(f'{skill.name}/SKILL.md missing YAML front matter fields')
    expected = {'gki-build', 'gki-ci', 'gki-gsi', 'gki-hal'}
    missing = expected - set(skill_names)
    if missing:
        fail(f'missing skills: {sorted(missing)}')
    for prompt in ('build.md', 'ci.md', 'gsi.md', 'hal.md', 'workbuddy.md'):
        require(ROOT / '.ai/prompts' / prompt)
    for alias in (ROOT / '.agents/skills', ROOT / '.claude/skills', ROOT / '.cursor/skills'):
        if not alias.exists():
            fail(f'missing skill alias dir {alias.relative_to(ROOT)}; run .ci/link-ai-skills.py')
            continue
        for name in expected:
            link = alias / name
            if not link.exists():
                fail(f'missing skill link {link.relative_to(ROOT)}')
            elif link.is_symlink():
                target = (link.parent / link.readlink()).resolve()
                if target != (skills_root / name).resolve():
                    fail(f'{link.relative_to(ROOT)} points to {target}')


def validate_ci_files() -> None:
    for path in (
        ROOT / '.gitignore',
        ROOT / '.github/workflows/build.yml',
        ROOT / '.ci/build-host.sh',
        ROOT / '.ci/build-android.sh',
        ROOT / '.ci/install-deps.sh',
        ROOT / '.ci/package-host.py',
        ROOT / '.ci/package-systems.py',
        ROOT / '.ci/package-android.py',
        ROOT / '.ci/fetch-guest-inputs.py',
        ROOT / '.ci/guest.lock.json',
        ROOT / 'scripts/ensure-guest-downloads.sh',
        ROOT / 'scripts/README.md',
        ROOT / '.ci/requirements.txt',
        ROOT / '.ci/README.md',
        ROOT / '.ai/README.md',
        ROOT / 'README.md',
        ROOT / 'AGENTS.md',
        ROOT / 'CLAUDE.md',
        ROOT / 'GEMINI.md',
        ROOT / '.ci/package-skills.py',
        ROOT / '.ci/link-ai-skills.py',
    ):
        require(path)
    workflow = ROOT / '.github/workflows/build.yml'
    if workflow.is_file():
        text = workflow.read_text()
        for needle in ('macos-26', 'ubuntu-24.04-arm', 'ubuntu-24.04',
                       '.ci/validate.py', '.ci/build-host.sh',
                       '.ci/package-systems.py', 'tebox-vendor-init',
                       'project-ai-skills', 'qemu-gki-'):
            if needle not in text:
                fail(f'workflow missing reference: {needle}')
        if 'contents: write' in text or 'permissions:\n  contents: write' in text:
            fail('workflow must stay contents: read for compile/upload-artifact')


def validate_gitignore() -> None:
    gi = ROOT / '.gitignore'
    if not gi.is_file():
        return
    text = gi.read_text()
    for pattern in ('/out/', '/dist/', '/downloads/', '.env',
                    '/toolchains/android-ndk', '/toolchains/android-sdk'):
        if pattern not in text:
            fail(f'.gitignore missing pattern: {pattern}')
    # These must remain trackable (not ignored). Local NDK/SDK symlinks are
    # machine paths and stay ignored; see toolchains/README.md.
    for banned in ('/prebuilts/', '/thirdparty/reference/',
                   '/toolchains/android-cmdline-tools',
                   '/toolchains/*-venv/',
                   '/src/aosp/*/qemu/busybox',
                   '/src/aosp/*/qemu/vendor/bin/',
                   '/src/aosp/*/qemu/vendor/lib/',
                   '/src/aosp/*/qemu/vendor/lib64/',
                   '**/keybox.xml', '*.keystore',
                   '/qemu/', '/thirdparty/mesa/src/',
                   '/src/aosp/*/system/*.img',
                   ):
        if any(line.strip() == banned for line in text.splitlines()):
            fail(f'.gitignore must not ignore tracked path: {banned}')
    # Rebuildable/runtime; must stay ignored (not uploaded).
    for needed in ('src/aosp/*/images/system.img',
                   'src/aosp/*/images/vendor.img',
                   'src/aosp/*/images/initramfs.img',
                   'src/kernel/*/gki/Image'):
        if needed not in text:
            fail(f'.gitignore missing rebuildable-image ignore: {needed}')
    guest = ROOT / '.ci/guest.lock.json'
    if guest.is_file():
        variant = json.loads(guest.read_text()).get('variant', '')
        require(ROOT / f'src/aosp/{variant}/images/README.md', 'variant images/README.md')
        require(ROOT / f'src/aosp/{variant}/prebuilts/gsi-lib64', 'variant prebuilts/gsi-lib64')
        require(ROOT / f'src/aosp/{variant}/hardware', 'variant hardware/')


def main() -> int:
    validate_guest_lock()
    validate_skills_and_prompts()
    validate_ci_files()
    validate_gitignore()
    if ERRORS:
        print('validate failed:', file=sys.stderr)
        for item in ERRORS:
            print(f'  - {item}', file=sys.stderr)
        return 1
    print('OK: guest pins, skills/prompts and CI wiring look consistent')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
