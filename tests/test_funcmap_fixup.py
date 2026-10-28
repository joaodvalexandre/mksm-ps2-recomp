"""Regressions of the function map fixup with a synthetic address space, without game files."""
import unittest

from tools.funcmap_fixup import JR_RA, data_code_pointers

TEXT = (0x100000, 0x100100)
DATA = (0x200000, 0x200020)


def space(words):
    return lambda address: words.get(address, 0)


class DataCodePointerTests(unittest.TestCase):
    def test_vtable_entry_after_a_return_is_an_entry(self):
        # a getter (jr $ra; lw) at 0x100040 and, right after it, a function only a vtable points at
        word = space({0x100040: JR_RA, 0x100044: 0x8C8200C0, 0x100048: 0x10A0000A, 0x200004: 0x100048})
        self.assertEqual(data_code_pointers([TEXT, DATA], TEXT, word, {0x100040}), {0x100048})

    def test_function_starts_and_mid_function_addresses_are_left_alone(self):
        word = space({0x100040: JR_RA, 0x100050: 0x24020001,
                      0x200000: 0x100048,   # already a function start
                      0x200008: 0x100054,   # inside a function, not after a return (a jump table case)
                      0x20000C: 0x100049})  # not aligned
        self.assertEqual(data_code_pointers([TEXT, DATA], TEXT, word, {0x100048}), set())

    def test_code_words_inside_text_are_not_pointers(self):
        word = space({0x100040: JR_RA, 0x100060: 0x100048})
        self.assertEqual(data_code_pointers([TEXT, DATA], TEXT, word, set()), set())


if __name__ == '__main__':
    unittest.main()
