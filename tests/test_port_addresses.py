"""Tests of tools/port_addresses.py: the instruction masking, the aligned search and the lui-pair reader."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.port_addresses import address_pairs, find_matches, mask_word  # noqa: E402


class FakeElf:
    """Enough of port_addresses.Elf for address_pairs: words at consecutive addresses."""

    def __init__(self, words, base=0x1000):
        self.words = words
        self.base = base

    def word(self, address):
        index = (address - self.base) // 4
        return self.words[index] if 0 <= index < len(self.words) else 0


class MaskWordTest(unittest.TestCase):
    def test_jump_targets_and_address_immediates_are_masked(self):
        self.assertEqual(mask_word(0x0C0F5420), 0x0C000000)  # jal: only the opcode stays
        self.assertEqual(mask_word(0x3C04004A), 0x3C040000)  # lui $a0, 0x4A: the half address goes
        self.assertEqual(mask_word(0x2484A9D0), 0x24840000)  # addiu $a0, $a0, -0x5630
        self.assertEqual(mask_word(0x8E420024), 0x8E420000)  # lw $v0, 0x24($s2)

    def test_registers_branches_and_cop_instructions_are_kept(self):
        self.assertEqual(mask_word(0x00A42823), 0x00A42823)  # subu
        self.assertEqual(mask_word(0x1440FFF9), 0x1440FFF9)  # bnez (relative offset)
        self.assertEqual(mask_word(0x4901FFFB), 0x4901FFFB)  # bc2t
        self.assertEqual(mask_word(0x46000842), 0x46000842)  # mul.s


class FindMatchesTest(unittest.TestCase):
    def test_only_aligned_unique_hits_are_reported(self):
        words = [0x11111111, 0x22222222, 0x33333333, 0x22222222, 0x33333333, 0x44444444]
        haystack = struct.pack('<6I', *words)
        needle = struct.pack('<2I', 0x22222222, 0x33333333)
        self.assertEqual(find_matches(haystack, needle), [1, 3])
        # a byte pattern that only occurs across a word boundary is not a match
        misaligned = struct.pack('<I', 0x33332222)
        self.assertEqual(find_matches(haystack, misaligned), [])


class AddressPairsTest(unittest.TestCase):
    def test_lui_pairs_resolve_to_absolute_addresses(self):
        words = [
            0x3C04004A,  # lui $a0, 0x4A
            0x2484A9D0,  # addiu $a0, $a0, -0x5630 -> 0x49A9D0
            0x3C020075,  # lui $v0, 0x75
            0x8C425DB8,  # lw $v0, 0x5DB8($v0) -> 0x755DB8
            0x3C031000,  # lui $v1, 0x1000
            0x3463E010,  # ori $v1, $v1, 0xE010 -> 0x1000E010
        ]
        pairs = address_pairs(FakeElf(words), 0x1000, 0x1000 + 4 * len(words))
        self.assertEqual(pairs, {0x4: 0x49A9D0, 0xC: 0x755DB8, 0x14: 0x1000E010})


if __name__ == '__main__':
    unittest.main()
