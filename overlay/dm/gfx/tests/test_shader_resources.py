"""Yaz0 回引与错误边界；不含游戏资源。
Yaz0 reference and malformed-input checks, with no game resources.
"""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split


def stream(size, body):
    return b'Yaz0' + struct.pack('>III', size, 0, 0) + body


class Yaz0Checks(unittest.TestCase):
    def test_literals(self):
        self.assertEqual(shader_split.yaz0_decode(stream(5, b'\xf8HELLO'), 'literal'), b'HELLO')

    def test_overlapping_short_reference(self):
        self.assertEqual(shader_split.yaz0_decode(stream(8, b'\xc0AB\x40\x01'), 'short'), b'ABABABAB')

    def test_long_reference(self):
        self.assertEqual(shader_split.yaz0_decode(stream(300, b'\x80A\x00\x00\xff\x00\x00\x08'), 'long'), b'A' * 300)

    def test_empty(self):
        self.assertEqual(shader_split.yaz0_decode(stream(0, b''), 'empty'), b'')

    def test_truncation(self):
        for data in (b'Yaz0', stream(3, b''), stream(3, b'\xffAB'), stream(20, b'\x00\x00\x00')):
            with self.subTest(data=data), self.assertRaises(ValueError):
                shader_split.yaz0_decode(data, 'truncated')

    def test_reference_before_history(self):
        with self.assertRaisesRegex(ValueError, 'before output'):
            shader_split.yaz0_decode(stream(3, b'\x00\x10\x00'), 'history')

    def test_reference_exceeds_declared_size(self):
        with self.assertRaisesRegex(ValueError, 'exceeds output size'):
            shader_split.yaz0_decode(stream(2, b'\x80A\x10\x00'), 'size')

    def test_headerless_compute_only(self):
        control = bytearray(0x880); control[:4] = shader_split.CTL_MAGIC
        code = bytes(32); struct.pack_into('<I', control, 0x704, len(code))
        self.assertIsNone(shader_split.check('raw compute', 'cs', control, code))
        self.assertIsNotNone(shader_split.check('invalid graphics', 'vs', control, code))
        self.assertIsNotNone(shader_split.check('short compute', 'cs', control, bytes(31)))

    def test_control_prefix_covers_compute_metadata(self):
        code = shader_split.GFX_MAGIC + bytes(28)
        control = bytearray(shader_split.CTL_HASH_BYTES)
        control[:4] = shader_split.CTL_MAGIC
        struct.pack_into('<I', control, 0x704, len(code))
        self.assertIsNone(shader_split.check('profile control', 'vs', control, code))
        self.assertIsNotNone(shader_split.check('truncated control', 'vs', control[:-1], code))

    def test_archive_name_stops_before_binary_payload(self):
        data = bytearray(0x80)
        data[:4] = b'SARC'
        struct.pack_into('<HHII', data, 4, 0x14, 0xfeff, len(data), 0x60)
        struct.pack_into('<H', data, 0x14 + 6, 1)
        struct.pack_into('<IIII', data, 0x14 + 12, 0, 0x01000000, 0, 0x20)
        data[0x38:0x3c] = b'abc\0'
        data[0x60:] = bytes(range(32))
        self.assertEqual(list(shader_split.sarc_members(data)), [('abc', bytes(range(32)))])


if __name__ == '__main__':
    unittest.main()
