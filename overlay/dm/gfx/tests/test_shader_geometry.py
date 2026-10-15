# 几何降级的 ABI 与拒绝路径回归；输入为人工 SPIR-V/MSL，不需要游戏资源。
# Geometry-lowering ABI and rejection regressions; synthetic SPIR-V/MSL inputs need no game resources.
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_geometry as geometry


class GeometryInterfaces(unittest.TestCase):
    def test_constant_register_and_reserved_binding_spaces(self):
        sets = geometry.argument_sets('''
struct spvDescriptorSetBuffer0 {
    constant Info* vb_info [[id(0)]];
    constant Guest* vp_c3 [[id(1)]];
    constant Guest* vp_c1 [[id(2)]];
};
struct spvDescriptorSetBuffer2 {
    texture_buffer<uint> ib_data [[id(0)]];
    texture_buffer<float> vb_data0 [[id(3)]];
};''')
        self.assertEqual([s['index'] for s in sets], [0, 2])
        self.assertEqual(sets[0]['bindings'][1]['reg'], 0)
        self.assertEqual(sets[0]['bindings'][2]['kind'], 'c1')
        self.assertEqual(sets[1]['words'], 4)
        self.assertEqual(sets[1]['bindings'][1]['location'], 0)

    def test_unknown_driver_or_guest_resource_is_rejected(self):
        for field in ('constant Guest* gp_c2', 'texture2d<float> guest_texture',
                      'texture_buffer<uint> vb_data0', 'device Data* mystery'):
            with self.subTest(field=field), self.assertRaises(ValueError):
                geometry.argument_sets('struct spvDescriptorSetBuffer0 { ' + field + ' [[id(0)]]; };')

    def test_duplicate_and_unknown_sets_are_rejected(self):
        for source in (
            'struct spvDescriptorSetBuffer0 { device X* vertex_output [[id(0)]]; device X* geometry_vb_output [[id(0)]]; };',
            'struct spvDescriptorSetBuffer3 { device X* vertex_output [[id(0)]]; };',
        ):
            with self.subTest(source=source), self.assertRaises(ValueError):
                geometry.argument_sets(source)

    def test_native_geometry_opcodes_cannot_escape_lowering(self):
        words = [0x07230203, 0x10000, 0, 2, 0, (4 << 16) | 15, 5, 1, 0]
        self.assertEqual(geometry.execution_model(struct.pack('<9I', *words)), 5)
        for opcode in (218, 219):
            with self.subTest(opcode=opcode), self.assertRaises(ValueError):
                geometry.execution_model(struct.pack('<10I', *words, (1 << 16) | opcode))
        for code in (b'', b'bad', struct.pack('<10I', *words, 0)):
            with self.subTest(code=code), self.assertRaises(ValueError):
                geometry.execution_model(code)

    def test_pair_identity_must_be_unambiguous(self):
        row = '0' * 16 + '\t' + '1' * 16 + '\t' + '2' * 16 + '\tSynthetic\n'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'pairs.tsv'
            path.write_text(row, encoding='utf8')
            self.assertEqual(geometry.pairs(path)[0]['source'], 'Synthetic')
            path.write_text(row + row, encoding='utf8')
            with self.assertRaises(ValueError): geometry.pairs(path)
            path.write_text(row.replace('0' * 16, '../foreign'), encoding='utf8')
            with self.assertRaises(ValueError): geometry.pairs(path)


if __name__ == '__main__': unittest.main()
