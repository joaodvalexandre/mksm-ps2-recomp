"""Regressions of the inspector with synthetic packets, without game files."""
import struct
import unittest

from tools.gs.inspect_packets import inspect


def tag(registers, nloop, fmt=0):
    nr = len(registers) & 15
    return struct.pack('<QQ', nloop | (1 << 15) | (fmt << 58) | (nr << 60),
                       sum(r << (4 * i) for i, r in enumerate(registers)))


class GifInspectorTests(unittest.TestCase):
    def test_packed_adc_and_xyz3(self):
        data = tag([5, 13], 2)
        for adc in [0, 0, 1, 1]:
            data += struct.pack('<QQ', 160 | (320 << 32), 12345 | (adc << 47))
        r = inspect(data)
        self.assertEqual((r['vertices'], r['vertices_with_kick'], r['vertices_without_kick']), (4, 1, 3))
        self.assertEqual(r['xyz_bounds'], [[160, 160], [320, 320], [12345, 12345]])

    def test_xyzf_fog_is_not_adc(self):
        data = tag([4], 1) + struct.pack('<QQ', 16 | (32 << 32), (0xabcdef << 4) | (255 << 36))
        r = inspect(data)
        self.assertEqual(r['vertices_with_kick'], 1)
        self.assertEqual(r['xyz_bounds'][2], [0xabcdef, 0xabcdef])

    def test_reglist_padding_and_next_tag(self):
        value = 16 | (32 << 16) | (123 << 32)
        data = tag([5], 1, 1) + struct.pack('<QQ', value, 0xffffffffffffffff)
        data += tag([13], 1, 1) + struct.pack('<QQ', value, 0)
        r = inspect(data)
        self.assertEqual((r['vertices'], r['vertices_with_kick']), (2, 1))
        self.assertEqual(r['xyz_bounds'], [[16, 16], [32, 32], [123, 123]])

    def test_ad_uses_register_coordinates(self):
        data = tag([14], 1) + struct.pack('<QQ', 16 | (32 << 16) | (456 << 32), 5)
        self.assertEqual(inspect(data)['xyz_bounds'], [[16, 16], [32, 32], [456, 456]])

    def test_nreg_zero_means_sixteen(self):
        data = tag([15] * 16, 1) + bytes(16 * 16)
        r = inspect(data)
        self.assertEqual(r['tags'][0]['nreg'], 16)
        self.assertEqual(r['vertices'], 0)

    def test_image_and_empty_tag(self):
        r = inspect(tag([15], 2, 2) + bytes(32) + tag([5], 0))
        self.assertEqual(len(r['tags']), 2)
        self.assertEqual(r['vertices'], 0)

    def test_pre_only_applies_in_packed(self):
        for fmt in [0, 1, 2]:
            data = bytearray(tag([15], 0, fmt))
            low, = struct.unpack_from('<Q', data)
            struct.pack_into('<Q', data, 0, low | (1 << 46) | (4 << 47))
            self.assertEqual(inspect(data)['prim_types_with_pre'], {4: 1} if fmt == 0 else {})

    def test_incomplete_payloads_and_unsupported_format(self):
        for data in [bytes(15), tag([5], 1), tag([5], 1, 1) + bytes(8),
                     tag([15], 1, 2), tag([15], 0, 3)]:
            with self.subTest(data=data), self.assertRaises(ValueError):
                inspect(data)


if __name__ == '__main__':
    unittest.main()
