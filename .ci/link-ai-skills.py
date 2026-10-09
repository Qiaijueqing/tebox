#!/usr/bin/env python3
"""Expose shared project skills to Codex/Cursor/Gemini and Claude Code."""
from pathlib import Path
import os

ROOT = Path(__file__).resolve().parent.parent
ALIASES = [
    ROOT / '.agents/skills',
    ROOT / '.claude/skills',
    ROOT / '.cursor/skills',
]


def main():
    skills = ROOT / '.ai/skills'
    for parent in ALIASES:
        parent.mkdir(parents=True, exist_ok=True)
        for source in sorted(skills.iterdir()):
            if not (source / 'SKILL.md').is_file():
                continue
            destination = parent / source.name
            if destination.exists() or destination.is_symlink():
                if destination.resolve() != source.resolve():
                    raise SystemExit(f'Refusing to overwrite {destination}')
                continue
            destination.symlink_to(os.path.relpath(source, parent), target_is_directory=True)
            print(destination.relative_to(ROOT))


if __name__ == '__main__':
    main()
