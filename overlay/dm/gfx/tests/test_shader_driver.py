import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from shader_driver import reads, unfilled


class DriverChecks(unittest.TestCase):
    def test_all_stages_and_helper_parameters(self):
        source = '(*spvDescriptorSet0.fp_c2)._m0[2u]; cp_c0._m0[46].x; vp_c0._m0[17]; gp_c2._m0[0x8];'
        self.assertEqual(reads(source), {
            '2': {'vec4_offsets': [32, 128], 'dynamic': False},
            '0': {'vec4_offsets': [272, 736], 'dynamic': False}})

    def test_dynamic_is_not_silently_proven_safe(self):
        self.assertEqual(unfilled(reads('cp_c0._m0[index];'), {0: [(0, 0xa00)]}), ['c0 dynamic index'])

    def test_names_and_regular_uniforms_are_not_driver_reads(self):
        self.assertEqual(reads('cp_c3._m0[46]; xcp_c0._m0[46];'), {})

    def test_other_stage_table_cannot_cover_current_stage(self):
        self.assertEqual(unfilled(reads('cp_c0._m0[17];'), {0: [(0x310, 0x410)]}), ['c0+0x110'])

    def test_partial_overlap_leaves_undefined_bytes(self):
        self.assertEqual(unfilled(reads('fp_c2._m0[22];'), {2: [(0x168, 0x568)]}), ['c2+0x160'])
        self.assertEqual(unfilled(reads('cp_c0._m0[46];'), {0: [(0x210, 0x310)]}), [])


if __name__ == '__main__':
    unittest.main()
