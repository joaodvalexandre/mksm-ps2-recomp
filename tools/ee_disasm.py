"""Disassembles a function (or an address range) of the game ELF with capstone, naming the targets of calls and
branches with the function map, and decoding the EE-only instructions capstone does not know (COP2 macro mode, MMI).

    python tools/ee_disasm.py game/SLUS_210.87 0x241410            # the function that starts there (config/funcmap.csv)
    python tools/ee_disasm.py game/SLUS_210.87 0x241410 0x2414e0   # an address range
    python tools/ee_disasm.py game/SLUS_210.87 sub_00241410        # by map name

Data references are not resolved; `lui`/`addiu` pairs are left to the reader. Requires `pip install capstone`."""
import csv
import os
import struct
import sys

try:
    import capstone
except ImportError:  # pragma: no cover
    sys.exit('pip install capstone')

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GPR = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
       's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
COP2_SPECIAL1 = {0x00: 'vaddx', 0x01: 'vaddy', 0x02: 'vaddz', 0x03: 'vaddw', 0x04: 'vsubx', 0x05: 'vsuby',
                 0x06: 'vsubz', 0x07: 'vsubw', 0x08: 'vmaddx', 0x09: 'vmaddy', 0x0A: 'vmaddz', 0x0B: 'vmaddw',
                 0x0C: 'vmsubx', 0x0D: 'vmsuby', 0x0E: 'vmsubz', 0x0F: 'vmsubw', 0x10: 'vmaxx', 0x11: 'vmaxy',
                 0x12: 'vmaxz', 0x13: 'vmaxw', 0x14: 'vminix', 0x15: 'vminiy', 0x16: 'vminiz', 0x17: 'vminiw',
                 0x18: 'vmulx', 0x19: 'vmuly', 0x1A: 'vmulz', 0x1B: 'vmulw', 0x1C: 'vmulq', 0x1D: 'vmaxi',
                 0x1E: 'vmuli', 0x1F: 'vminii', 0x20: 'vaddq', 0x21: 'vmaddq', 0x22: 'vaddi', 0x23: 'vmaddi',
                 0x24: 'vsubq', 0x25: 'vmsubq', 0x26: 'vsubi', 0x27: 'vmsubi', 0x28: 'vadd', 0x29: 'vmadd',
                 0x2A: 'vmul', 0x2B: 'vmax', 0x2C: 'vsub', 0x2D: 'vmsub', 0x2E: 'vopmsub', 0x2F: 'vmini',
                 0x30: 'viadd', 0x31: 'visub', 0x32: 'viaddi', 0x34: 'viand', 0x35: 'vior', 0x38: 'vcallms',
                 0x39: 'vcallmsr'}
COP2_SPECIAL2 = {0x00: 'vaddax', 0x01: 'vadday', 0x02: 'vaddaz', 0x03: 'vaddaw', 0x04: 'vsubax', 0x05: 'vsubay',
                 0x06: 'vsubaz', 0x07: 'vsubaw', 0x08: 'vmaddax', 0x09: 'vmadday', 0x0A: 'vmaddaz', 0x0B: 'vmaddaw',
                 0x0C: 'vmsubax', 0x0D: 'vmsubay', 0x0E: 'vmsubaz', 0x0F: 'vmsubaw', 0x10: 'vitof0', 0x11: 'vitof4',
                 0x12: 'vitof12', 0x13: 'vitof15', 0x14: 'vftoi0', 0x15: 'vftoi4', 0x16: 'vftoi12', 0x17: 'vftoi15',
                 0x18: 'vmulax', 0x19: 'vmulay', 0x1A: 'vmulaz', 0x1B: 'vmulaw', 0x1C: 'vmulaq', 0x1D: 'vabs',
                 0x1E: 'vmulai', 0x1F: 'vclipw', 0x20: 'vaddaq', 0x21: 'vmaddaq', 0x22: 'vaddai', 0x23: 'vmaddai',
                 0x24: 'vsubaq', 0x25: 'vmsubaq', 0x26: 'vsubai', 0x27: 'vmsubai', 0x28: 'vadda', 0x29: 'vmadda',
                 0x2A: 'vmula', 0x2C: 'vsuba', 0x2D: 'vmsuba', 0x2E: 'vopmula', 0x2F: 'vnop', 0x30: 'vmove',
                 0x31: 'vmr32', 0x34: 'vlqi', 0x35: 'vsqi', 0x36: 'vlqd', 0x37: 'vsqd', 0x38: 'vdiv', 0x39: 'vsqrt',
                 0x3A: 'vrsqrt', 0x3B: 'vwaitq', 0x3C: 'vmtir', 0x3D: 'vmfir', 0x3E: 'vilwr', 0x3F: 'viswr',
                 0x40: 'vrnext', 0x41: 'vrget', 0x42: 'vrinit', 0x43: 'vrxor'}
MMI_NAMES = {0x00: 'madd', 0x01: 'maddu', 0x04: 'plzcw', 0x08: 'mmi0', 0x09: 'mmi2', 0x10: 'mfhi1', 0x11: 'mthi1',
             0x12: 'mflo1', 0x13: 'mtlo1', 0x18: 'mult1', 0x19: 'multu1', 0x1A: 'div1', 0x1B: 'divu1',
             0x20: 'madd1', 0x21: 'maddu1', 0x28: 'mmi1', 0x29: 'mmi3', 0x30: 'pmfhl', 0x31: 'pmthl',
             0x34: 'psllh', 0x36: 'psrlh', 0x37: 'psrah', 0x3C: 'psllw', 0x3E: 'psrlw', 0x3F: 'psraw'}


def load_map(path):
    starts = {}
    names = {}
    with open(path, newline='') as handle:
        for row in csv.DictReader(handle):
            start = int(row['Start'], 16)
            starts[start] = (row['Name'], int(row['End'], 16))
            names[row['Name']] = start
    return starts, names


def load_elf(path):
    data = open(path, 'rb').read()
    assert data[:4] == b'\x7fELF', 'not an ELF'
    phoff, = struct.unpack_from('<I', data, 28)
    phentsize, phnum = struct.unpack_from('<HH', data, 42)
    segments = []
    for i in range(phnum):
        ptype, offset, vaddr, _paddr, filesz, memsz = struct.unpack_from('<IIIIII', data, phoff + i * phentsize)
        if ptype == 1 and filesz:
            segments.append((vaddr, offset, filesz))
    return data, segments


def read_bytes(data, segments, address, size):
    for vaddr, offset, filesz in segments:
        if vaddr <= address < vaddr + filesz:
            end = min(address + size, vaddr + filesz)
            return data[offset + address - vaddr: offset + end - vaddr]
    raise SystemExit('address 0x%X is not in the file image' % address)


def fields(word):
    return (word >> 26) & 0x3F, (word >> 21) & 0x1F, (word >> 16) & 0x1F, (word >> 11) & 0x1F, (word >> 6) & 0x1F, word & 0x3F


def dest_mask(word):
    mask = (word >> 21) & 0xF
    return ''.join(c for c, bit in zip('xyzw', (8, 4, 2, 1)) if mask & bit)


def decode_special(word):
    """The instructions capstone's MIPS64 decoder refuses on the EE: COP2 macro mode, MMI, and a few others."""
    op, rs, rt, rd, sa, fn = fields(word)
    if op == 0x12:  # COP2
        if rs == 0x01:
            return 'qmfc2 $%s, vf%d' % (GPR[rt], rd)
        if rs == 0x02:
            return 'cfc2 $%s, vi%d' % (GPR[rt], rd)
        if rs == 0x05:
            return 'qmtc2 $%s, vf%d' % (GPR[rt], rd)
        if rs == 0x06:
            return 'ctc2 $%s, vi%d' % (GPR[rt], rd)
        if rs == 0x08:
            return 'bc2%s %d' % ({0: 'f', 1: 't', 2: 'fl', 3: 'tl'}.get(rt, '?'), (word & 0xFFFF))
        if rs >= 0x10:
            if fn >= 0x3C:
                sub = ((word & 0x7C0) >> 4) | (word & 3)
                name = COP2_SPECIAL2.get(sub, 'cop2.s2(0x%02X)' % sub)
            else:
                name = COP2_SPECIAL1.get(fn, 'cop2.s1(0x%02X)' % fn)
            return '%s.%s vf%d, vf%d, vf%d' % (name, dest_mask(word) or '-', (word >> 6) & 0x1F, (word >> 11) & 0x1F,
                                               (word >> 16) & 0x1F)
    if op == 0x1C:  # MMI
        return '%s' % MMI_NAMES.get(fn, 'mmi(0x%02X)' % fn) + ' $%s, $%s, $%s' % (GPR[rd], GPR[rs], GPR[rt])
    if op == 0x1E:
        return 'lq $%s, %d($%s)' % (GPR[rt], struct.unpack('<h', struct.pack('<H', word & 0xFFFF))[0], GPR[rs])
    if op == 0x1F:
        return 'sq $%s, %d($%s)' % (GPR[rt], struct.unpack('<h', struct.pack('<H', word & 0xFFFF))[0], GPR[rs])
    if op == 0x31:
        return 'lwc1 $f%d, %d($%s)' % (rt, struct.unpack('<h', struct.pack('<H', word & 0xFFFF))[0], GPR[rs])
    if op == 0x39:
        return 'swc1 $f%d, %d($%s)' % (rt, struct.unpack('<h', struct.pack('<H', word & 0xFFFF))[0], GPR[rs])
    if op == 0x36:
        return 'lqc2 vf%d, %d($%s)' % (rt, struct.unpack('<h', struct.pack('<H', word & 0xFFFF))[0], GPR[rs])
    if op == 0x3E:
        return 'sqc2 vf%d, %d($%s)' % (rt, struct.unpack('<h', struct.pack('<H', word & 0xFFFF))[0], GPR[rs])
    if word == 0x0000000F:
        return 'sync'
    return '.word 0x%08X' % word


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    elf_path = argv[1]
    starts, names = load_map(os.path.join(REPO, 'config', 'funcmap.csv'))
    where = argv[2]
    start = names[where] if where in names else int(where, 16)
    if len(argv) > 3:
        end = int(argv[3], 16)
    elif start in starts:
        end = starts[start][1]
    else:
        end = start + 0x100
    data, segments = load_elf(elf_path)
    code = read_bytes(data, segments, start, end - start)
    md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 | capstone.CS_MODE_LITTLE_ENDIAN)
    md.skipdata = True
    md.skipdata_setup = ('.word', None, None)
    print('; %s 0x%X-0x%X' % (starts.get(start, ('?', 0))[0], start, end))
    for insn in md.disasm(code, start):
        word, = struct.unpack_from('<I', code, insn.address - start)
        text = '%s %s' % (insn.mnemonic, insn.op_str)
        if insn.mnemonic == '.word' or (word >> 26) in (0x12, 0x1C, 0x1E, 0x1F, 0x36, 0x3E):
            text = decode_special(word)
        if insn.mnemonic in ('jal', 'j'):
            target = ((insn.address + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
            text += '    ; %s' % starts.get(target, ('?', 0))[0]
        print('%08X  %08X  %s' % (insn.address, word, text))


if __name__ == '__main__':
    main(sys.argv)
