#!/usr/bin/env python3
"""Completes the analyzer's function map and entry points for a stripped ELF.

ps2_analyzer finds functions from JAL targets, so it misses code that is only reached by a J (tail call) and does
not register jumps into the middle of another function (shared epilogues). At run time both end in
"[guest-branch:missing-target]". This tool:

  1. adds a sub_XXXXXXXX entry to config/funcmap.csv for every gap of .text that holds code;
  2. adds to `entry_points` of config/recomp.template.toml every J/JAL target that is not a function start
     (the recompiler turns those into resume entries of the owning function, or into a standalone function).

Usage:  python tools/funcmap_fixup.py game/SLES_535.24 [--check]
        --check only reports, and fails if something is missing.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FUNCMAP = ROOT / 'config' / 'funcmap.csv'
TEMPLATE = ROOT / 'config' / 'recomp.template.toml'


def load_elf(path: Path):
    data = path.read_bytes()
    phoff, = struct.unpack_from('<I', data, 0x1C)
    phnum, = struct.unpack_from('<H', data, 0x2C)
    shoff, = struct.unpack_from('<I', data, 0x20)
    shnum, shstrndx = struct.unpack_from('<HH', data, 0x30)
    segments = []
    for i in range(phnum):
        p_type, p_off, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from('<8I', data, phoff + i * 32)
        if p_type == 1:
            segments.append((p_vaddr, p_off, p_filesz))
    names_off = struct.unpack_from('<10I', data, shoff + shstrndx * 40)[4]
    text = None
    for i in range(shnum):
        name, _, _, addr, _, size = struct.unpack_from('<6I', data, shoff + i * 40)
        end = data.index(b'\0', names_off + name)
        if data[names_off + name:end] == b'.text':
            text = (addr, addr + size)
    if text is None:
        sys.exit(f'{path}: no .text section')

    def word(address: int) -> int:
        for vaddr, offset, size in segments:
            if vaddr <= address < vaddr + size:
                return struct.unpack_from('<I', data, offset + address - vaddr)[0]
        return 0

    return text, word


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument('elf', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()

    (text_start, text_end), word = load_elf(args.elf)
    with FUNCMAP.open(newline='', encoding='utf-8') as f:
        rows = list(csv.DictReader(f))
    funcs = sorted((int(r['Start'], 16), int(r['End'], 16)) for r in rows)

    # 1. Gaps of .text with non-zero words.
    gaps = []
    cursor = text_start
    for start, end in funcs:
        if start > cursor and any(word(a) for a in range(cursor, start, 4)):
            gaps.append((cursor, start))
        cursor = max(cursor, end)
    funcs = sorted(funcs + gaps)
    starts = [s for s, _ in funcs]
    start_set = set(starts)

    def owner(address: int):
        i = bisect.bisect_right(starts, address) - 1
        return funcs[i] if i >= 0 and funcs[i][0] <= address < funcs[i][1] else None

    # 2. J/JAL targets that are not function starts (a J inside its own function is an ordinary branch).
    targets = set()
    for address in range(text_start, text_end, 4):
        instruction = word(address)
        opcode = instruction >> 26
        if opcode not in (2, 3):
            continue
        target = ((address + 4) & 0xF0000000) | ((instruction & 0x03FFFFFF) << 2)
        if target in start_set or not text_start <= target < text_end:
            continue
        if opcode == 2 and owner(address) is not None and owner(address) == owner(target):
            continue
        targets.add(target)

    template = TEMPLATE.read_text(encoding='utf-8')
    block = re.search(r'^entry_points = \[\n(.*?)^\]', template, re.M | re.S)
    if block is None:
        sys.exit(f'{TEMPLATE}: no entry_points list')
    listed = {int(m, 16) for m in re.findall(r'@0x([0-9A-Fa-f]+)"', block.group(1))}
    missing = sorted(targets - listed)

    print(f'funcmap gaps with code: {len(gaps)}; jump targets missing from entry_points: {len(missing)}')
    if args.check:
        return 1 if gaps or missing else 0

    if gaps:
        for start, end in gaps:
            rows.append({'Name': f'sub_{start:08X}', 'Start': f'0x{start:08X}', 'End': f'0x{end:08X}',
                         'Size': str(end - start)})
        rows.sort(key=lambda r: int(r['Start'], 16))
        with FUNCMAP.open('w', newline='', encoding='utf-8') as f:
            writer = csv.DictWriter(f, fieldnames=['Name', 'Start', 'End', 'Size'], lineterminator='\n')
            writer.writeheader()
            writer.writerows(rows)
    if missing:
        added = ''.join(f'  "sub_{t:08X}@0x{t:08X}",\n' for t in missing)
        body = block.group(1)
        if body and not body.rstrip().endswith(','):
            body = body.rstrip('\n') + ',\n'
        template = template[:block.start(1)] + body + added + template[block.end(1):]
        TEMPLATE.write_text(template, encoding='utf-8', newline='\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
