#!/usr/bin/env python3
"""Validates config/funcmap.csv and config/recomp.template.toml without needing the game.

Usage:  python tools/ci/validate_config.py [--runtime PATH_TO_PS2RECOMP]

Errors (exit code 1), which would otherwise only show up when recompiling on Windows:
  - funcmap.csv: header, hexadecimal addresses, order, overlaps, Size != End - Start, repeated names.
  - recomp.template.toml: @ELF@/@MAP@/@OUT@ markers, "name@0xADDRESS" format in stubs and
    entry_points, addresses outside every function of funcmap.csv.
  - With --runtime: stubs without a handler in ps2xRuntime/include/ps2_call_list.h (the name, or without a
    leading "_").
Warnings (do not fail): stubs that do not start at a function (e.g. those that start in a delay slot and are
linked from src/gow_overrides.cpp) and addresses repeated between stubs and entry_points.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FUNCMAP = ROOT / "config" / "funcmap.csv"
TEMPLATE = ROOT / "config" / "recomp.template.toml"
LIBM_DOUBLE = {"sin", "cos", "tan", "atan", "atan2", "sqrt", "pow", "exp", "log", "fabs", "floor", "ceil", "fmod"}
SELECTOR = re.compile(r"^(?P<name>[A-Za-z_$][\w$.]*)@0x(?P<addr>[0-9A-Fa-f]{1,8})$")

errors: list[str] = []
warnings: list[str] = []


def read_funcmap() -> list[tuple[str, int, int]]:
    with FUNCMAP.open(newline="", encoding="utf-8") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header != ["Name", "Start", "End", "Size"]:
            errors.append(f"funcmap.csv: header {header}, expected Name,Start,End,Size")
            return []
        functions = []
        for n, row in enumerate(reader, start=2):
            try:
                name, start, end, size = row
                start_i, end_i, size_i = int(start, 16), int(end, 16), int(size)
            except ValueError:
                errors.append(f"funcmap.csv:{n}: malformed row: {row}")
                continue
            if end_i - start_i != size_i:
                errors.append(f"funcmap.csv:{n}: {name}: Size {size_i} != End - Start ({end_i - start_i})")
            if end_i <= start_i:
                errors.append(f"funcmap.csv:{n}: {name}: End <= Start")
            functions.append((name, start_i, end_i))

    seen: dict[str, int] = {}
    for i, (name, start, end) in enumerate(functions):
        if name in seen:
            errors.append(f"funcmap.csv: repeated name {name}")
        seen[name] = start
        if i and start < functions[i - 1][1]:
            errors.append(f"funcmap.csv: {name} (0x{start:08X}) is not sorted by address")
        elif i and start < functions[i - 1][2]:
            errors.append(f"funcmap.csv: {name} (0x{start:08X}) overlaps {functions[i - 1][0]}")
    return functions


def selectors(config: dict, key: str) -> list[tuple[str, int]]:
    values = config.get("general", {}).get(key, [])
    result = []
    for v in values:
        m = SELECTOR.match(v) if isinstance(v, str) else None
        if not m:
            errors.append(f"recomp.template.toml: {key}: '{v}' does not have the name@0xADDRESS format")
            continue
        result.append((m["name"], int(m["addr"], 16)))
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runtime", type=Path, help="PS2Recomp checkout (with the patches applied)")
    args = ap.parse_args()

    functions = read_funcmap()
    starts = [f[1] for f in functions]

    text = TEMPLATE.read_text(encoding="utf-8")
    for marker in ("@ELF@", "@MAP@", "@OUT@"):
        if marker not in text:
            errors.append(f"recomp.template.toml: missing the {marker} marker that build.ps1 fills in")
    try:
        config = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        errors.append(f"recomp.template.toml: invalid TOML: {e}")
        config = {}

    stubs = selectors(config, "stubs")
    entries = selectors(config, "entry_points")

    for key, items in (("stubs", stubs), ("entry_points", entries)):
        for name, address in items:
            i = bisect.bisect_right(starts, address) - 1
            if i < 0 or address >= functions[i][2]:
                errors.append(f"{key}: {name}@0x{address:08X} does not fall inside any function of funcmap.csv")
            elif key == "stubs" and address != functions[i][1]:
                warnings.append(f"stubs: {name}@0x{address:08X} does not start a function (it is inside {functions[i][0]})")

    # The game's libm is a software double ($a0 -> $v0); the runtime handlers are float
    # ($f12 -> $f0). Replacing it returns the argument unchanged (the sine table came out as sin(x) = x).
    for name, address in stubs:
        if name in LIBM_DOUBLE:
            errors.append(f"stubs: {name}@0x{address:08X} is the software double libm; "
                          "the runtime's float handler does not respect its ABI")

    by_address: dict[int, list[str]] = {}
    for name, address in stubs + entries:
        by_address.setdefault(address, []).append(name)
    for address, names in sorted(by_address.items()):
        if len(names) > 1:
            warnings.append(f"0x{address:08X} appears several times: {', '.join(names)}")

    if args.runtime:
        call_list = args.runtime / "ps2xRuntime" / "include" / "ps2_call_list.h"
        handlers = set(re.findall(r"X\((\w+)\)", call_list.read_text(encoding="utf-8")))
        for name, address in stubs:
            if name not in handlers and name.lstrip("_") not in handlers:
                errors.append(f"stubs: {name}@0x{address:08X} has no handler in ps2_call_list.h")

    for w in warnings:
        print(f"::warning::{w}")
    for e in errors:
        print(f"::error::{e}")
    print(f"funcmap.csv: {len(functions)} functions; stubs: {len(stubs)}; entry_points: {len(entries)}; "
          f"errors: {len(errors)}; warnings: {len(warnings)}")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
