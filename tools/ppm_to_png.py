#!/usr/bin/env python3
"""Converts the runtime's PPM/PGM captures (gs_hw_*.ppm, *_a.pgm alpha planes) to PNG, which Windows can open.

usage: python tools/ppm_to_png.py [--delete] <file or folder> [...]

Each PNG is written next to its source; --delete removes the source afterwards. For every file it prints the size and
how much of it is not black, which is often all a capture needs to tell (an empty render target, a blank texture).
"""
from __future__ import annotations

import argparse
import struct
import sys
import zlib
from pathlib import Path


def read_netpbm(path: Path) -> tuple[int, int, int, bytes]:
    """Returns (width, height, channels, pixels) of a binary P6 (RGB) or P5 (grey) file."""
    data = path.read_bytes()
    fields: list[bytes] = []
    pos = 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    pos += 1  # the single whitespace after maxval
    kinds = {b'P6': 3, b'P5': 1}
    if fields[0] not in kinds:
        raise ValueError(f'{path}: not a binary PPM/PGM ({fields[0]!r})')
    width, height, channels = int(fields[1]), int(fields[2]), kinds[fields[0]]
    pixels = data[pos:pos + width * height * channels]
    if len(pixels) < width * height * channels:
        raise ValueError(f'{path}: truncated')
    return width, height, channels, pixels


def write_png(path: Path, width: int, height: int, channels: int, pixels: bytes) -> None:
    stride = width * channels
    raw = b''.join(b'\x00' + pixels[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (struct.pack('>I', len(payload)) + kind + payload +
                struct.pack('>I', zlib.crc32(kind + payload) & 0xFFFFFFFF))

    colour_type = 2 if channels == 3 else 0
    header = struct.pack('>IIBBBBB', width, height, 8, colour_type, 0, 0, 0)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(raw, 6)) +
                     chunk(b'IEND', b''))


def convert(path: Path, delete: bool) -> str:
    width, height, channels, pixels = read_netpbm(path)
    write_png(path.with_suffix('.png'), width, height, channels, pixels)
    total = width * height
    if channels == 3:
        lit = sum(1 for i in range(0, len(pixels), 3) if pixels[i] | pixels[i + 1] | pixels[i + 2])
    else:
        lit = sum(1 for value in pixels if value)
    if delete:
        path.unlink()
    return f'{path.with_suffix(".png").name}: {width}x{height}, {100.0 * lit / max(1, total):.2f}% not black'


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--delete', action='store_true', help='remove each source after converting it')
    parser.add_argument('paths', nargs='+', type=Path)
    args = parser.parse_args(argv)
    files: list[Path] = []
    for path in args.paths:
        files += sorted(p for p in path.iterdir() if p.suffix in ('.ppm', '.pgm')) if path.is_dir() else [path]
    for path in files:
        print(convert(path, args.delete))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
