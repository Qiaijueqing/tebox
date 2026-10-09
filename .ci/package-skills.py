#!/usr/bin/env python3
"""Create portable SKILL.md ZIP packages for WorkBuddy's local skill importer."""
from pathlib import Path
import zipfile

ROOT = Path(__file__).resolve().parent.parent


def main():
    out = ROOT / 'dist/skills'
    out.mkdir(parents=True, exist_ok=True)
    for skill in sorted((ROOT / '.ai/skills').iterdir()):
        if not (skill / 'SKILL.md').is_file():
            continue
        destination = out / (skill.name + '.zip')
        with zipfile.ZipFile(destination, 'w', zipfile.ZIP_DEFLATED) as archive:
            for file in sorted(skill.rglob('*')):
                if file.is_file():
                    archive.write(file, file.relative_to(skill.parent))
        print(destination.relative_to(ROOT))


if __name__ == '__main__':
    main()
