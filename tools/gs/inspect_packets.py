#!/usr/bin/env python3
"""Summarizes positions and ADC of local GIF captures, without needing the game."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct


def inspect(data: bytes) -> dict:
    """Validates the bounds and counts vertices; a kick does not guarantee a primitive."""
    position = 0
    vertices = []
    tags = []
    prim_types = Counter()

    def require(size: int) -> None:
        if size > len(data) - position:
            raise ValueError(f"incomplete payload at 0x{position:x}: {size - (len(data) - position)} bytes missing")

    def xyz(register: int, low: int, high: int, packed: bool) -> None:
        if register not in (4, 5, 12, 13):
            return
        x = low & 0xffff
        y = (low >> (32 if packed else 16)) & 0xffff
        z = ((high >> 4) & 0xffffff if register in (4, 12) else high & 0xffffffff) if packed else (
            (low >> 32) & (0xffffff if register in (4, 12) else 0xffffffff))
        no_kick = register in (12, 13) or (packed and bool((high >> 47) & 1))
        vertices.append((x, y, z, no_kick))

    while position < len(data):
        require(16)
        low, high = struct.unpack_from('<QQ', data, position)
        position += 16
        nloop = low & 0x7fff
        fmt = (low >> 58) & 3
        nreg = (low >> 60) & 15 or 16
        registers = [(high >> (4 * i)) & 15 for i in range(nreg)]
        tags.append({'nloop': nloop, 'format': fmt, 'nreg': nreg})
        if fmt == 0 and (low >> 46) & 1:
            prim_types[(low >> 47) & 7] += 1
        if fmt == 0:
            require(nloop * nreg * 16)
            for i in range(nloop * nreg):
                v0, v1 = struct.unpack_from('<QQ', data, position)
                position += 16
                register = registers[i % nreg]
                xyz(register, v0, v1, True)
                if register == 14:  # A+D uses the layout of a 64-bit GS register.
                    xyz(v1 & 255, v0, 0, False)
        elif fmt == 1:
            require(((nloop * nreg * 8 + 15) // 16) * 16)
            for i in range(nloop * nreg):
                v0, = struct.unpack_from('<Q', data, position)
                position += 8
                xyz(registers[i % nreg], v0, 0, False)
            if (nloop * nreg) & 1:
                position += 8
        elif fmt == 2:
            require(nloop * 16)
            position += nloop * 16
        else:
            raise ValueError(f"GIF format 3 not supported at 0x{position - 16:x}")

    result = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
              'tags': tags, 'prim_types_with_pre': dict(prim_types),
              'vertices': len(vertices), 'vertices_with_kick': sum(not v[3] for v in vertices),
              'vertices_without_kick': sum(v[3] for v in vertices),
              'distinct_xyz': len({v[:3] for v in vertices})}
    if vertices:
        result['xyz_bounds'] = [[min(v[i] for v in vertices), max(v[i] for v in vertices)] for i in range(3)]
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path, help='GIF file or folder with mksm_geo_gif_*.bin')
    parser.add_argument('--json', type=Path, help='save the report, preferably under logs/')
    args = parser.parse_args()
    files = [args.capture] if args.capture.is_file() else sorted(
        args.capture.glob('mksm_geo_gif_*.bin'), key=lambda p: int(re.search(r'_(\d+)$', p.stem)[1]))
    if not files:
        parser.error('no GIF captures were found')
    rows = []
    for file in files:
        try:
            rows.append({'file': file.name, **inspect(file.read_bytes())})
        except ValueError as error:
            parser.error(f'{file.name}: {error}')
    report = {'packets': len(rows), 'vertices': sum(r['vertices'] for r in rows),
              'vertices_with_kick': sum(r['vertices_with_kick'] for r in rows),
              'vertices_without_kick': sum(r['vertices_without_kick'] for r in rows),
              'packets_without_vertices': sum(r['vertices'] == 0 for r in rows), 'details': rows}
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k != 'details'}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
