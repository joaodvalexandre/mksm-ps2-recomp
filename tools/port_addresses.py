#!/usr/bin/env python3
"""Finds the addresses of another build of the game from the addresses of a known one.

The two executables (for example the PAL and the NTSC release) hold the same functions at different addresses. Each
function is fingerprinted by its instruction words with the position-dependent fields masked out (J/JAL targets and
the 16-bit immediates of the instructions that carry halves of absolute addresses), and the fingerprint is searched
in the other executable's .text. A hit that is unique is reported with the matching function of the other map, and
the absolute addresses the two functions build with lui/addiu, lui/ori or lui/load-store pairs are listed side by
side, which is how a data address (a flag, a table, a float) is ported too.

usage: python tools/port_addresses.py <from.elf> <from_funcmap.csv> <to.elf> <to_funcmap.csv> <addr> [<addr>...]
       python tools/port_addresses.py ... --data <addr> [<addr>...]   # data addresses: port through their users

<addr> is hexadecimal (0x prefix optional). For a code address inside a function the port is the same offset inside
the matched function. --data looks for the functions that build the address with a lui pair, ports them and reads
the constant the other build uses at the same instruction.
"""
from __future__ import annotations

import argparse
import bisect
import csv
import struct
import sys
from pathlib import Path

OP_J, OP_JAL = 2, 3
BRANCH_OPS = {1, 4, 5, 6, 7, 20, 21, 22, 23}
# I-type instructions whose immediate is the half of an absolute address or a data offset
IMMEDIATE_OPS = {8, 9, 10, 11, 12, 13, 14, 15, 24, 25} | set(range(32, 64))


def mask_word(w: int) -> int:
    """The position-independent part of an instruction word."""
    op = w >> 26
    if op in (OP_J, OP_JAL):
        return w & 0xFC000000
    if op in IMMEDIATE_OPS:
        return w & 0xFFFF0000
    return w  # R-type, branches (relative), COP1/COP2, MMI


class Elf:
    def __init__(self, path: Path):
        self.data = path.read_bytes()
        d = self.data
        phoff, = struct.unpack_from('<I', d, 0x1C)
        phnum, = struct.unpack_from('<H', d, 0x2C)
        shoff, = struct.unpack_from('<I', d, 0x20)
        shnum, shstrndx = struct.unpack_from('<HH', d, 0x30)
        self.segments = []
        for i in range(phnum):
            p_type, p_off, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from('<8I', d, phoff + i * 32)
            if p_type == 1:
                self.segments.append((p_vaddr, p_off, p_filesz))
        names_off = struct.unpack_from('<10I', d, shoff + shstrndx * 40)[4]
        self.text = None
        for i in range(shnum):
            name, _, _, addr, _, size = struct.unpack_from('<6I', d, shoff + i * 40)
            end = d.index(b'\0', names_off + name)
            if d[names_off + name:end] == b'.text':
                self.text = (addr, addr + size)
        if self.text is None:
            sys.exit(f'{path}: no .text section')
        self.entry, = struct.unpack_from('<I', d, 0x18)
        start, end = self.text
        self.text_bytes = self.read(start, end - start)
        words = struct.unpack_from(f'<{len(self.text_bytes) // 4}I', self.text_bytes)
        self.masked = struct.pack(f'<{len(words)}I', *(mask_word(w) for w in words))

    def read(self, address: int, size: int) -> bytes:
        for vaddr, offset, filesz in self.segments:
            if vaddr <= address < vaddr + filesz:
                return self.data[offset + address - vaddr: offset + address - vaddr + size]
        return b''

    def word(self, address: int) -> int:
        raw = self.read(address, 4)
        return struct.unpack('<I', raw)[0] if len(raw) == 4 else 0


class FuncMap:
    def __init__(self, path: Path):
        self.functions: list[tuple[int, int, str]] = []
        with path.open(newline='', encoding='utf-8') as f:
            reader = csv.reader(f)
            next(reader, None)
            for name, start, end, _size in reader:
                self.functions.append((int(start, 16), int(end, 16), name))
        self.functions.sort()
        self.starts = [s for s, _, _ in self.functions]

    def containing(self, address: int):
        i = bisect.bisect_right(self.starts, address) - 1
        if i >= 0 and self.functions[i][0] <= address < self.functions[i][1]:
            return self.functions[i]
        return None

    def at(self, start: int):
        i = bisect.bisect_left(self.starts, start)
        return self.functions[i] if i < len(self.functions) and self.functions[i][0] == start else None


def find_matches(haystack: bytes, needle: bytes) -> list[int]:
    """Word offsets of every aligned occurrence of needle in haystack (both are packed masked words)."""
    hits = []
    pos = haystack.find(needle)
    while pos != -1:
        if pos % 4 == 0:
            hits.append(pos // 4)
        pos = haystack.find(needle, pos + 1)
    return hits


def fingerprint(elf: Elf, start: int, end: int, max_words: int = 48) -> bytes:
    size = min(end - start, max_words * 4)
    return elf.masked[(start - elf.text[0]):(start - elf.text[0]) + size]


def sign16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def address_pairs(elf: Elf, start: int, end: int) -> dict[int, int]:
    """offset -> absolute address built by a lui and the next instruction that completes it with the same register."""
    pairs = {}
    words = [elf.word(a) for a in range(start, end, 4)]
    for i, w in enumerate(words):
        if (w >> 26) != 0x0F:
            continue
        rt = (w >> 16) & 31
        hi = (w & 0xFFFF) << 16
        for j in range(i + 1, min(i + 6, len(words))):
            u = words[j]
            op, rs, urt = u >> 26, (u >> 21) & 31, (u >> 16) & 31
            if op == 9 and rs == rt:  # addiu
                pairs[j * 4] = (hi + sign16(u & 0xFFFF)) & 0xFFFFFFFF
                break
            if op == 13 and rs == rt:  # ori
                pairs[j * 4] = hi | (u & 0xFFFF)
                break
            if op >= 32 and rs == rt:  # load / store
                pairs[j * 4] = (hi + sign16(u & 0xFFFF)) & 0xFFFFFFFF
                break
            if urt == rt or ((u >> 11) & 31) == rt and op == 0:  # the register is overwritten
                break
    return pairs


def port_function(src: Elf, src_map: FuncMap, dst: Elf, dst_map: FuncMap, start: int, end: int):
    needle = fingerprint(src, start, end)
    if len(needle) < 16:
        return None, 'function too short to fingerprint'
    hits = find_matches(dst.masked, needle)
    if len(hits) != 1:
        # try a longer fingerprint before giving up
        needle = fingerprint(src, start, end, max_words=200)
        hits = find_matches(dst.masked, needle)
        if len(hits) != 1:
            return None, f'{len(hits)} matches'
    return dst.text[0] + hits[0] * 4, 'unique'


def describe(src: Elf, src_map: FuncMap, dst: Elf, dst_map: FuncMap, address: int) -> None:
    fn = src_map.containing(address)
    if fn is None:
        print(f'{address:#010x}: not inside any function of the source map')
        return
    start, end, name = fn
    target, status = port_function(src, src_map, dst, dst_map, start, end)
    if target is None:
        print(f'{address:#010x} ({name} {start:#x}-{end:#x}): no port ({status})')
        return
    dst_fn = dst_map.at(target) or dst_map.containing(target)
    dst_name = f'{dst_fn[2]} {dst_fn[0]:#x}-{dst_fn[1]:#x}' if dst_fn else 'no function starts there in the target map'
    ported = target + (address - start)
    print(f'{address:#010x} ({name} +{address - start:#x}) -> {ported:#010x}  [{dst_name}]')
    src_pairs = address_pairs(src, start, end)
    dst_pairs = address_pairs(dst, target, target + (end - start))
    shown = 0
    for offset, value in sorted(src_pairs.items()):
        other = dst_pairs.get(offset)
        if other is not None and other != value:
            print(f'    +{offset:#x}: {value:#010x} -> {other:#010x}')
            shown += 1
    if not shown:
        print('    (no differing absolute addresses built inside the function)')


def users_of(elf: Elf, fmap: FuncMap, data_address: int, limit: int = 4):
    """Functions that build data_address with a lui pair, with the instruction offset."""
    found = []
    for start, end, name in fmap.functions:
        for offset, value in address_pairs(elf, start, end).items():
            if value == data_address:
                found.append((start, end, name, offset))
                break
        if len(found) >= limit:
            break
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('from_elf', type=Path)
    parser.add_argument('from_map', type=Path)
    parser.add_argument('to_elf', type=Path)
    parser.add_argument('to_map', type=Path)
    parser.add_argument('addresses', nargs='+')
    parser.add_argument('--data', action='store_true', help='the addresses are data: port them through their users')
    args = parser.parse_args()
    src, dst = Elf(args.from_elf), Elf(args.to_elf)
    src_map, dst_map = FuncMap(args.from_map), FuncMap(args.to_map)
    print(f'source .text {src.text[0]:#x}-{src.text[1]:#x} entry {src.entry:#x}; '
          f'target .text {dst.text[0]:#x}-{dst.text[1]:#x} entry {dst.entry:#x}')
    for item in args.addresses:
        address = int(item, 16)
        if not args.data:
            describe(src, src_map, dst, dst_map, address)
            continue
        users = users_of(src, src_map, address)
        if not users:
            print(f'data {address:#010x}: no function builds it with a lui pair')
            continue
        votes: dict[int, int] = {}
        for start, end, name, offset in users:
            target, status = port_function(src, src_map, dst, dst_map, start, end)
            if target is None:
                print(f'data {address:#010x}: user {name} {start:#x} has no port ({status})')
                continue
            other = address_pairs(dst, target, target + (end - start)).get(offset)
            print(f'data {address:#010x}: user {name} {start:#x}+{offset:#x} -> {target:#x}: '
                  f'{"%#010x" % other if other is not None else "no pair at that offset"}')
            if other is not None:
                votes[other] = votes.get(other, 0) + 1
        if votes:
            best = max(votes.items(), key=lambda kv: kv[1])
            print(f'data {address:#010x} -> {best[0]:#010x}  ({best[1]} of {len(users)} users agree)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
