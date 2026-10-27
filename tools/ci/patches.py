#!/usr/bin/env python3
"""Prints, in order, the patches that scripts/build.ps1 applies on top of PS2Recomp.

CI applies them in the same order as the Windows build, without a duplicated list that could fall out of date.
It fails if a patch in patches/ is not applied or if a file is missing.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD_SCRIPT = ROOT / "scripts" / "build.ps1"


def main() -> int:
    text = BUILD_SCRIPT.read_text(encoding="utf-8-sig")
    # $variable = Join-Path $RepoRoot 'patches\file.patch'
    paths = {m[1].lower(): m[2] for m in re.finditer(
        r"^\s*\$(\w+)\s*=\s*Join-Path\s+\$RepoRoot\s+'(patches[\\/][^']+\.patch)'", text, re.M | re.I)}
    # Run $git @('apply', ..., $variable)
    order = [m[1].lower() for m in re.finditer(
        r"^\s*Run\s+\$git\s+@\(\s*'apply'[^)]*\$(\w+)\s*\)", text, re.M | re.I)]

    errors = []
    patches = []
    for var in order:
        if var not in paths:
            errors.append(f"build.ps1 applies ${var}, but its path was not found")
            continue
        path = ROOT / paths[var].replace("\\", "/")
        if not path.is_file():
            errors.append(f"{path.relative_to(ROOT).as_posix()} does not exist")
        patches.append(path)
    unapplied = sorted(set((ROOT / "patches").glob("*.patch")) - set(patches))
    for path in unapplied:
        errors.append(f"{path.relative_to(ROOT).as_posix()} is not applied in build.ps1")
    if not patches:
        errors.append("no patch was found in build.ps1")

    for e in errors:
        print(f"::error::{e}", file=sys.stderr)
    if errors:
        return 1
    for path in patches:
        print(path.as_posix())
    return 0


if __name__ == "__main__":
    sys.exit(main())
