"""Lists the call and jump targets in the game ELF that the recompiled code cannot reach: a jal or j, present in
the generated code, whose target is not registered in the runtime's dispatch table (neither a function start nor a
resume label of one). Such a transfer ends the game thread at run time with "[guest-branch:missing-target]".

    python tools/missing_targets.py game/SLES_535.24

Run it after a build: it reads PS2Recomp/ps2xRuntime/src/runner (one file per function, and register_functions.cpp,
whose table entries are commented with the guest address they serve). Only direct jal/j are checked; jr/jalr
targets are dynamic. Fix a reported target with an entry point in config/recomp.toml (and, for a library
function the runtime implements, a stub binding at the same address)."""
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(REPO, 'PS2Recomp', 'ps2xRuntime', 'src', 'runner')


def load_functions():
    files = {}
    for entry in os.listdir(RUNNER):
        if entry.endswith('.cpp') and '_0x' in entry:
            name, address = entry[:-4].rsplit('_0x', 1)
            try:
                files[int(address, 16)] = (name, os.path.join(RUNNER, entry))
            except ValueError:
                pass
    return files


def load_registered():
    registered = set()
    with open(os.path.join(RUNNER, 'register_functions.cpp'), encoding='utf-8', errors='replace') as handle:
        for match in re.finditer(r'=\s*\w+;\s*//\s*0x([0-9a-fA-F]+)', handle.read()):
            registered.add(int(match.group(1), 16))
    return registered


def load_text(path):
    data = open(path, 'rb').read()
    phoff = struct.unpack_from('<I', data, 0x1C)[0]
    phnum = struct.unpack_from('<H', data, 0x2C)[0]
    for i in range(phnum):
        p_type, p_off, p_vaddr, _, p_filesz, _, p_flags, _ = struct.unpack_from('<IIIIIIII', data, phoff + 32 * i)
        if p_type == 1 and (p_flags & 1):
            return p_vaddr, data[p_off:p_off + p_filesz]
    raise SystemExit('no executable segment')


def main(argv):
    elf = argv[1] if len(argv) > 1 else os.path.join(REPO, 'game', 'SLES_535.24')
    functions = load_functions()
    registered = load_registered()
    ordered = sorted(functions)
    base, text = load_text(elf)
    end = base + len(text)

    def containing(address):
        lo, hi, best = 0, len(ordered) - 1, None
        while lo <= hi:
            mid = (lo + hi) // 2
            if ordered[mid] <= address:
                best = ordered[mid]
                lo = mid + 1
            else:
                hi = mid - 1
        return best

    generated_cache = {}

    def in_generated_code(function_start, pc):
        # The generated code comments every translated instruction ("// 0x1234:"); a stub's body or a range the
        # recompiler did not translate has no such line for pc.
        if function_start not in generated_cache:
            with open(functions[function_start][1], encoding='utf-8', errors='replace') as handle:
                generated_cache[function_start] = handle.read()
        return ('// 0x%x:' % pc) in generated_cache[function_start]

    missing = {}
    for offset in range(0, len(text) - 3, 4):
        word = struct.unpack_from('<I', text, offset)[0]
        op = word >> 26
        if op not in (0x02, 0x03):
            continue
        pc = base + offset
        target = ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
        if target < base or target >= end or target in registered:
            continue
        source = containing(pc)
        if source is None:
            continue
        if op == 0x02 and containing(target) == source:
            continue  # a jump inside its own function
        if not in_generated_code(source, pc):
            continue
        missing.setdefault(target, []).append((pc, 'jal' if op == 0x03 else 'j', functions[source][0]))

    for target in sorted(missing):
        sites = missing[target]
        inside = containing(target)
        first = struct.unpack_from('<I', text, target - base)[0]
        print('0x%08X  first word %08X  inside %s+0x%X  referenced %d time(s): %s' % (
            target, first, functions[inside][0] if inside is not None else '?', target - (inside or 0), len(sites),
            ', '.join('%s at 0x%X in %s' % (kind, pc, fn) for pc, kind, fn in sites[:4])))
    print('%d unreachable targets in generated code' % len(missing))


if __name__ == '__main__':
    main(sys.argv)
