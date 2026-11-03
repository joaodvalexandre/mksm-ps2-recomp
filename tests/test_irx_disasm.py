"""Regressions of the IRX disassembler with hand-built words and a synthetic module, without game files."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))   # also under python -I, which drops the cwd

from tools.iop.irx_disasm import IrxModule, Listing, decode, ordinal_name  # noqa: E402

BASE = 0x10000
R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6


def words(*values):
    return struct.pack(f'<{len(values)}I', *values)


# A module with a "sifcmd" import table (stubs 17 and 22), a function that registers an RPC server, the handler it
# names, a .data pointer to that handler and a .bss buffer. Addresses are module offsets; the relocations relocate
# them to the load base.
TEXT = (
    words(0x41E00000, 0, 0x00000101, 0x63666973, 0x0000646D) +  # import table of "sifcmd"       0x00
    words(0x03E00008, 0x24000011, 0x03E00008, 0x24000016, 0, 0) +  # stubs 17 and 22, end     0x14..0x2b
    words(0x27BDFFE0, 0xAFBF001C,                                  # main: addiu sp; sw ra      0x2c
          0x3C040000, 0x24840090,                                  # lui/addiu a0 = sd (.data)   0x34 HI16/LO16
          0x3C058000, 0x34A50701,                                  # lui/ori a1 = 0x80000701     0x3c
          0x3C060000, 0x24C60070,                                  # lui/addiu a2 = handler      0x44 HI16/LO16
          0x3C070000, 0x24E700B0,                                  # lui/addiu a3 = buff (.bss)  0x4c HI16/LO16
          0xAFA00010, 0xAFA00014,                                  # cfunc = cbuff = 0           0x54
          0x0C000005, 0x00000000,                                  # jal stub 17; nop            0x5c R_MIPS_26
          0x8FBF001C, 0x03E00008, 0x27BD0020,                      # lw ra; jr ra; addiu sp      0x64
          0x3C080001, 0x8D088010,                                  # handler: lui/lw t0 = [0x8010] 0x70 HI16/LO16
          0x1500FFFD, 0x00000000,                                  # bnez t0, handler; nop       0x78
          0x03E00008, 0x00001021,                                  # jr ra; move v0, zero        0x80
          0, 0))                                                   # padding                     0x88
DATA = words(0x70, 0, 0, 0, 0, 0, 0, 0)                            # 0x90: a pointer to the handler
RELOCS_TEXT = [(0x34, R_MIPS_HI16), (0x38, R_MIPS_LO16), (0x44, R_MIPS_HI16), (0x48, R_MIPS_LO16),
               (0x4C, R_MIPS_HI16), (0x50, R_MIPS_LO16), (0x5C, R_MIPS_26), (0x70, R_MIPS_HI16),
               (0x74, R_MIPS_LO16)]
RELOCS_DATA = [(0x90, R_MIPS_32)]
ENTRY = 0x2C


def build_irx():
    """A minimal relocatable IOP ELF (e_type 0xFF80) with .iopmod, .text, .rel.text, .data, .rel.data and .bss."""
    iopmod = struct.pack('<6IH', 0xFFFFFFFF, ENTRY, 0, len(TEXT), len(DATA), 0x20, 0x0102) + b'test_module\0'
    rel_text = b''.join(struct.pack('<II', off, typ) for off, typ in RELOCS_TEXT)
    rel_data = b''.join(struct.pack('<II', off, typ) for off, typ in RELOCS_DATA)
    shstrtab = b'\0.iopmod\0.text\0.rel.text\0.data\0.rel.data\0.bss\0.shstrtab\0.symtab\0.strtab\0'
    symtab, strtab = bytes(16), b'\0'
    blobs = [iopmod, TEXT + DATA, rel_text, rel_data, shstrtab, symtab, strtab]
    offsets, pos = [], 52 + 32
    for blob in blobs:
        pos = (pos + 15) & ~15
        offsets.append(pos)
        pos += len(blob)
    shoff = (pos + 3) & ~3
    text_off = offsets[1]
    names = {n: shstrtab.index(b'\0' + n + b'\0') + 1 for n in
             (b'.iopmod', b'.text', b'.rel.text', b'.data', b'.rel.data', b'.bss', b'.shstrtab', b'.symtab', b'.strtab')}
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (names[b'.iopmod'], 0x70000080, 0, 0, offsets[0], len(iopmod), 0, 0, 4, 0),
        (names[b'.text'], 1, 6, 0, text_off, len(TEXT), 0, 0, 16, 0),
        (names[b'.rel.text'], 9, 0, 0, offsets[2], len(rel_text), 8, 2, 4, 8),
        (names[b'.data'], 1, 3, len(TEXT), text_off + len(TEXT), len(DATA), 0, 0, 16, 0),
        (names[b'.rel.data'], 9, 0, 0, offsets[3], len(rel_data), 8, 4, 4, 8),
        (names[b'.bss'], 8, 3, len(TEXT) + len(DATA), text_off + len(TEXT) + len(DATA), 0x20, 0, 0, 16, 0),
        (names[b'.shstrtab'], 3, 0, 0, offsets[4], len(shstrtab), 0, 0, 1, 0),
        (names[b'.symtab'], 2, 0, 0, offsets[5], len(symtab), 9, 1, 4, 16),
        (names[b'.strtab'], 3, 0, 0, offsets[6], len(strtab), 0, 0, 1, 0),
    ]
    header = b'\x7fELF\x01\x01\x01' + bytes(9) + struct.pack('<HHIIIIIHHHHHH', 0xFF80, 8, 1, ENTRY, 52, shoff, 1, 52,
                                                               32, 1, 40, len(sections), 7)
    phdr = struct.pack('<8I', 1, text_off, 0, 0, len(TEXT) + len(DATA), len(TEXT) + len(DATA) + 0x20, 7, 16)
    image = bytearray(header + phdr)
    for off, blob in zip(offsets, blobs):
        image += bytes(off - len(image)) + blob
    image += bytes(shoff - len(image))
    for s in sections:
        image += struct.pack('<10I', *s)
    return bytes(image)


class DecodeTests(unittest.TestCase):
    def test_r_type(self):
        ins = decode(0x00851021, 0)
        self.assertEqual((ins.mnemonic, ins.operands, ins.writes), ('addu', '$v0, $a0, $a1', 2))
        self.assertEqual(decode(0x0062102B, 0).operands, '$v0, $v1, $v0')      # sltu
        self.assertEqual(decode(0x03E00008, 0).mnemonic + ' ' + decode(0x03E00008, 0).operands, 'jr $ra')
        self.assertTrue(decode(0x03E00008, 0).is_jump)
        self.assertEqual(decode(0x00001021, 0).mnemonic, 'move')
        self.assertEqual(decode(0, 0).mnemonic, 'nop')

    def test_i_type(self):
        self.assertEqual(decode(0x8E460010, 0).operands, '$a2, 0x10($s2)')      # lw
        self.assertEqual(decode(0x27BDFFC8, 0).operands, '$sp, $sp, -0x38')     # addiu
        self.assertEqual(decode(0x2411FFFA, 0).mnemonic + ' ' + decode(0x2411FFFA, 0).operands, 'li $s1, -6')
        self.assertEqual(decode(0x3C02534E, 0).operands, '$v0, 0x534e')         # lui
        ins = decode(0x14400008, 0x75FF0)                                       # bnez $v0, +8
        self.assertEqual((ins.mnemonic, ins.target, ins.is_jump, ins.is_call), ('bnez', 0x76014, True, False))
        self.assertEqual(decode(0x1500FFFD, 0x78).target, 0x70)                 # backwards

    def test_j_type_and_cop0(self):
        ins = decode(0x0C000092, 0xE158)
        self.assertEqual((ins.mnemonic, ins.target, ins.is_call, ins.writes), ('jal', 0x248, True, 31))
        self.assertEqual(decode(0x08000010, 0x10000000).target, 0x10000040)     # j keeps the 256 MB region
        self.assertEqual(decode(0x40026000, 0).operands, '$v0, $SR')            # mfc0
        self.assertEqual(decode(0x40826000, 0).mnemonic, 'mtc0')
        self.assertEqual(decode(0x42000010, 0).mnemonic, 'rfe')


class ModuleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = IrxModule(build_irx(), BASE)
        cls.listing = Listing(cls.module)
        cls.text = cls.listing.render()

    def test_iopmod(self):
        m = self.module
        self.assertEqual((m.name, m.version, m.entry, m.text_size), ('test_module', 0x102, ENTRY, len(TEXT)))

    def test_relocations(self):
        m = self.module
        self.assertEqual(m.word(BASE + 0x34), 0x3C040001)       # lui $a0, hi(0x10090)
        self.assertEqual(m.word(BASE + 0x38), 0x24840090)       # addiu $a0, lo
        self.assertEqual(m.word(BASE + 0x5C), 0x0C004005)       # jal 0x10014
        self.assertEqual(m.word(BASE + 0x70), 0x3C080002)       # a negative lo16 carries into the hi16
        self.assertEqual(m.word(BASE + 0x74), 0x8D088010)
        self.assertEqual(m.word(BASE + 0x90), BASE + 0x70)      # the .data pointer
        self.assertEqual(m.relocations, len(RELOCS_TEXT) + len(RELOCS_DATA) - 4)   # HI16 entries count with their LO16

    def test_import_table(self):
        tables = self.module.imports
        self.assertEqual([t.library for t in tables], ['sifcmd'])
        self.assertEqual(tables[0].stubs, {BASE + 0x14: 17, BASE + 0x1C: 22})
        self.assertEqual(self.module.stub_name(BASE + 0x14), 'sifcmd:17 sceSifRegisterRpc')
        self.assertEqual(self.module.exports, [])

    def test_ordinal_names(self):
        self.assertEqual(ordinal_name('foo', 9), 'foo:9')
        self.assertEqual(ordinal_name('libsd', 17), 'libsd:17 sceSdVoiceTrans')
        self.assertEqual(ordinal_name('libsd', 31), 'libsd:31 sceSdSetEffectWorkArea?')
        self.assertEqual(ordinal_name('LIBSD', 2), 'LIBSD:2 sceSdQuit')

    def test_listing_annotations(self):
        self.assertIn(f'{BASE + 0x5c:08x}: 0c004005  jal     0x10014              ; sifcmd:17 sceSifRegisterRpc',
                      self.text)
        self.assertIn('ori     $a1, $a1, 0x701      ; = 0x80000701', self.text)
        self.assertIn('addiu   $a2, $a2, 0x70       ; = 0x10070 func_10070', self.text)
        self.assertIn('lw      $t0, -0x7ff0($t0)    ; = 0x18010', self.text)
        self.assertIn('\n_start:\n', self.text)
        self.assertIn('\nstub_sifcmd_17:\n', self.text)
        self.assertIn('\nfunc_10070:\n', self.text)                  # a handler, from the .data pointer and the call
        self.assertIn('; import table of "sifcmd": magic', self.text)
        self.assertIn('0x10090 (.data) -> 0x10070 func_10070', self.text)

    def test_rpc_heuristic(self):
        servers = [c for c in self.listing.calls if c[3] == 'RPC servers']
        self.assertEqual(len(servers), 1)
        address, key, name, section, values = servers[0]
        self.assertEqual((address, key), (BASE + 0x5C, ('sifcmd', 17)))
        self.assertEqual(values, {'sd': BASE + 0x90, 'sid': 0x80000701, 'func': BASE + 0x70, 'buff': BASE + 0xB0,
                                  'cfunc': 0, 'cbuff': 0, 'qd': None})
        self.assertIn('sceSifRegisterRpc(sd=0x10090, sid=0x80000701, func=0x10070 (func_10070), buff=0x100b0, '
                      'cfunc=0x0, cbuff=0x0, qd=?)', self.text)


if __name__ == '__main__':
    unittest.main()
