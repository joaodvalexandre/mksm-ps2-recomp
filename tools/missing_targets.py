"""Lists the call and jump targets in the game ELF that the recompiled code cannot reach: a jal or j, present in
the generated code, whose target is not registered in the runtime's dispatch table (neither a function start nor a
resume label of one). Such a transfer ends the game thread at run time with "[guest-branch:missing-target]".

    python tools/missing_targets.py game/SLES_535.24
    python tools/missing_targets.py game/SLES_535.24 --data-pointers [--entry-points]

Run it after a build: it reads PS2Recomp/ps2xRuntime/src/runner (one file per function, and register_functions.cpp,
whose table entries are commented with the guest address they serve). Only direct jal/j are checked; jr/jalr
targets are dynamic. Fix a reported target with an entry point in config/recomp.toml (and, for a library
function the runtime implements, a stub binding at the same address).

--data-pointers scans the data sections (.data, .rodata...) for words that point into .text but at no registered
address. In Shaolin Monks most of them are the pc_begin of .eh_frame records (the unwind tables, linked into .data);
the rest are function pointers of vtables and callback tables, which the game calls with jalr: an unregistered one
stops the game thread just the same. Only pointers that look like a function start are listed: the target allocates a
stack frame (addiu $sp, $sp, -n) or follows a return (jr $ra and its delay slot, then up to three words of nop
padding), or another unconditional jump (j, b, jr to a register: the end of a tail call). A pointer whose neighbour in
the table points into the same function, at neither of the first two kinds of start, is taken for a jump table (switch
cases, which end with jumps) and left out.
--entry-points prints the result as config/recomp.toml entry points instead."""
import os
import re
import struct
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNNER = os.path.join(REPO, 'PS2Recomp', 'ps2xRuntime', 'src', 'runner')


JR_RA = 0x03E00008  # jr $ra


def is_unconditional_jump(word):
    """jr rs, j target, or b offset (beq $zero, $zero)."""
    return (word & 0xFC1FFFFF) == 0x00000008 or (word >> 26) == 0x02 or (word >> 16) == 0x1000


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


def load_segments(path):
    """The PT_LOAD segments as (vaddr, bytes, executable)."""
    data = open(path, 'rb').read()
    phoff = struct.unpack_from('<I', data, 0x1C)[0]
    phnum = struct.unpack_from('<H', data, 0x2C)[0]
    segments = []
    for i in range(phnum):
        p_type, p_off, p_vaddr, _, p_filesz, _, p_flags, _ = struct.unpack_from('<IIIIIIII', data, phoff + 32 * i)
        if p_type == 1 and p_filesz:
            segments.append((p_vaddr, data[p_off:p_off + p_filesz], bool(p_flags & 1)))
    return segments


def load_sections(path):
    """The allocated sections with contents as (name, vaddr, bytes, executable); empty without section headers."""
    data = open(path, 'rb').read()
    shoff = struct.unpack_from('<I', data, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', data, 0x2E)
    if not shoff or not shnum:
        return []
    headers = [struct.unpack_from('<IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
    names = headers[shstrndx][4]
    sections = []
    for name, sh_type, flags, addr, offset, size, _, _, _, _ in headers:
        if addr and size and sh_type == 1 and (flags & 2):  # SHT_PROGBITS, SHF_ALLOC
            label = data[names + name:data.index(b'\0', names + name)].decode()
            sections.append((label, addr, data[offset:offset + size], bool(flags & 4)))
    return sections


def load_text(path):
    # The code is .text when the ELF has section headers (Shaolin Monks has one segment for code and data).
    for name, vaddr, contents, _ in load_sections(path):
        if name == '.text':
            return vaddr, contents
    for vaddr, contents, executable in load_segments(path):
        if executable:
            return vaddr, contents
    raise SystemExit('no executable segment')


def containing_function(ordered, address):
    lo, hi, best = 0, len(ordered) - 1, None
    while lo <= hi:
        mid = (lo + hi) // 2
        if ordered[mid] <= address:
            best = ordered[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def direct_targets(functions, registered, base, text):
    ordered = sorted(functions)
    end = base + len(text)
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
        source = containing_function(ordered, pc)
        if source is None:
            continue
        if op == 0x02 and containing_function(ordered, target) == source:
            continue  # a jump inside its own function
        if not in_generated_code(source, pc):
            continue
        missing.setdefault(target, []).append((pc, 'jal' if op == 0x03 else 'j', functions[source][0]))

    for target in sorted(missing):
        sites = missing[target]
        inside = containing_function(ordered, target)
        first = struct.unpack_from('<I', text, target - base)[0]
        print('0x%08X  first word %08X  inside %s+0x%X  referenced %d time(s): %s' % (
            target, first, functions[inside][0] if inside is not None else '?', target - (inside or 0), len(sites),
            ', '.join('%s at 0x%X in %s' % (kind, pc, fn) for pc, kind, fn in sites[:4])))
    print('%d unreachable targets in generated code' % len(missing))


def data_pointers(functions, registered, sections, base, text, as_entry_points):
    ordered = sorted(functions)
    end = base + len(text)

    def word_at(address):
        return struct.unpack_from('<I', text, address - base)[0]

    def follows(address, is_jump):
        # a jump and its delay slot, then 0 to 3 words of nop padding, right before address
        for padding in range(4):
            if padding and word_at(address - 4 * padding) != 0:
                break
            jump = address - 4 * padding - 8
            if jump >= base and is_jump(word_at(jump)):
                return True
        return False

    def strong_start(address):
        first = word_at(address)
        # addiu $sp, $sp, -n (opcode 9, rs = rt = 29, negative immediate)
        if (first >> 16) == 0x27BD and (first & 0x8000):
            return 'allocates a frame'
        if follows(address, lambda word: word == JR_RA):
            return 'after a return'
        return None

    def weak_start(address):
        return 'after a jump' if follows(address, is_unconditional_jump) else None

    def in_text(value):
        return base <= value < end and not value & 3

    found = {}
    skipped_tables = 0
    for vaddr, contents, executable in sections:
        if executable:
            continue  # instruction words are not pointers
        words = [struct.unpack_from('<I', contents, offset)[0] for offset in range(0, len(contents) - 3, 4)]
        for index, value in enumerate(words):
            if not in_text(value) or value in registered:
                continue
            home = containing_function(ordered, value)
            neighbours = [words[i] for i in (index - 1, index + 1) if 0 <= i < len(words)]
            same_function = [n for n in neighbours if in_text(n) and containing_function(ordered, n) == home]
            reason = strong_start(value) or weak_start(value)
            if not reason:
                continue
            if any(n not in registered and not strong_start(n) for n in same_function):
                skipped_tables += 1
                continue
            found.setdefault(value, (reason, []))[1].append(vaddr + 4 * index)

    if as_entry_points:
        for target in sorted(found):
            print('  "mksm_ptr_%06x@0x%08X",' % (target, target))
        return
    for target in sorted(found):
        reason, sites = found[target]
        inside = containing_function(ordered, target)
        print('0x%08X  %-17s inside %s+0x%X  pointed to from %d word(s): %s' % (
            target, reason, functions[inside][0] if inside is not None else '?', target - (inside or 0), len(sites),
            ', '.join('0x%X' % site for site in sites[:4])))
    print('%d function pointers to unregistered code (%d jump table entries left out)' % (len(found), skipped_tables))


def main(argv):
    args = [arg for arg in argv[1:] if not arg.startswith('--')]
    elf = args[0] if args else os.path.join(REPO, 'game', 'SLES_535.24')
    functions = load_functions()
    registered = load_registered()
    base, text = load_text(elf)
    if '--data-pointers' in argv:
        sections = [(vaddr, contents, executable) for _, vaddr, contents, executable in load_sections(elf)]
        data_pointers(functions, registered, sections or load_segments(elf), base, text, '--entry-points' in argv)
    else:
        direct_targets(functions, registered, base, text)


if __name__ == '__main__':
    main(sys.argv)
