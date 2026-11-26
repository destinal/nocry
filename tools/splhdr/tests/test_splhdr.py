"""Tests for nocry_splhdr. Run: python3 -m unittest discover -s tools/splhdr/tests

Optional: NOCRY_BLOB_SPL=/path/to/ballaswag/spl.bin enables the byte-for-byte
check against the header Ingenic's cloner produced.
"""
import json
import os
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import nocry_splhdr as s  # noqa: E402

PROFILES = os.path.join(os.path.dirname(HERE), "profiles")


class GoldenCCreator(unittest.TestCase):
    """The Python port must equal Ingenic's C creator bit for bit."""

    def test_w97bv6mk_matches_c_creator(self):
        with open(os.path.join(HERE, "golden_c_creator_w97bv6mk.json")) as f:
            g = json.load(f)["values"]
        for freq, exp in g.items():
            with self.subTest(freq=freq):
                cfg = s.DdrConfig(chip="W97BV6MK", freq=int(freq), odt=True,
                                  auto_self_refresh=True, actpd=False, mr3_ds=2)
                r = s.compute_ddr_regs(cfg)
                self.assertEqual(r.id, exp["h.id"])
                self.assertEqual(s.DDR_TYPE_ENUM["v12"][r.type_name], exp["h.type"])
                self.assertEqual(r.freq, exp["h.freq"])
                for k in s.REG_ORDER:
                    self.assertEqual(r.regs[k], exp[k], f"{k} @ {freq}: {r.regs[k]:#x} != {exp[k]:#x}")
                for i in range(5):
                    self.assertEqual(r.remap[i], exp[f"REMMAP_ARRAY[{i}]"])


# Register values in ballaswag's spl.bin header (Ingenic cloner, 400 MHz).
BLOB_DDR = {
    "DDRC_CFG": 0x82004a29, "DDRC_CTRL": 0x0000b092, "DDRC_MMAP0": 0x20f0, "DDRC_MMAP1": 0x3000,
    "DDRC_REFCNT": 0x40610081, "DDRC_TIMING1": 0x030a0603, "DDRC_TIMING2": 0x040c0306,
    "DDRC_TIMING3": 0x03080409, "DDRC_TIMING4": 0x141a1104, "DDRC_TIMING5": 0x0e062033,
    "DDRC_AUTOSR_CNT": 0x1a000101, "DDRC_AUTOSR_EN": 0, "DDRC_HREGPRO": 1, "DDRC_PREGPRO": 1,
    "DDRC_CGUC0": 0x11111111, "DDRC_CGUC1": 0x113, "DDRP_MEMCFG": 0x13, "DDRP_CL": 6,
    "DDRP_CWL": 3, "DDR_MR0": 0, "DDR_MR1": 0x183, "DDR_MR2": 0x204, "DDR_MR3": 0x302,
    "DDR_MR10": 0xaff, "DDR_MR11": 0, "DDR_MR63": 0x3f00, "DDR_CHIP_0_SIZE": 0x10000000,
    "DDR_CHIP_1_SIZE": 0,
}


class ClonerBlob(unittest.TestCase):
    def setUp(self):
        prof = s.load_profile(os.path.join(PROFILES, "ingenic-cloner-x2000e-400mhz.json"))
        self.cfg, self.chip, self.board, self.layout, _ = s.profile_to_objects(prof)

    def test_registers_match_blob(self):
        r = s.compute_ddr_regs(self.cfg, self.chip)
        for k, v in BLOB_DDR.items():
            self.assertEqual(r.regs[k], v, f"{k}: {r.regs[k]:#x} != {v:#x}")
        self.assertEqual(r.remap, [0x030f0e0d, 0x07060504, 0x0b0a0908, 0x0201000c, 0x13121110])

    @unittest.skipUnless(os.environ.get("NOCRY_BLOB_SPL"), "set NOCRY_BLOB_SPL to ballaswag spl.bin")
    def test_header_bytes_match_blob(self):
        with open(os.environ["NOCRY_BLOB_SPL"], "rb") as f:
            blob = f.read()
        hdr = s.build_header(self.board, s.compute_ddr_regs(self.cfg, self.chip), self.layout)
        # Everything the SPL parses is identical; 0x180.. holds unparsed leftovers in the blob.
        self.assertEqual(hdr[:0x180], blob[:0x180])


class K1Profile(unittest.TestCase):
    def setUp(self):
        prof = s.load_profile(os.path.join(PROFILES, "creality-x2000e.json"))
        self.cfg, self.chip, self.board, self.layout, self.mpll = s.profile_to_objects(prof)
        self.hdr = s.build_header(self.board, s.compute_ddr_regs(self.cfg, self.chip), self.layout)

    def test_structure(self):
        tl = list(s.parse_tlvs(self.hdr))
        self.assertEqual([m for _, m, _, _ in tl], [s.MAGIC_INFO, s.MAGIC_DDR])
        self.assertEqual(tl[0][2], 0x94)          # sizeof(global_info), v12 SPL DWARF
        self.assertEqual(tl[1][2], 0xd4)          # sizeof(ddr_reg_value), v12 SPL DWARF
        self.assertEqual(len(self.hdr), 0x800)
        self.assertEqual(self.hdr[:4], b"FIDB")
        end = 8 + 0x94 + 8 + 0xd4
        self.assertEqual(self.hdr[end:end + 8], bytes(8))   # terminator

    def test_v12_enum_and_freq(self):
        _, _, _, ddr = list(s.parse_tlvs(self.hdr))[1]
        name, did, dtype, freq, regs, _ = s.decode_ddr(ddr)
        self.assertEqual(dtype, 3)                # LPDDR2 in the v12 enum (DDR4 first)
        self.assertEqual(freq, 500_000_000)
        self.assertEqual(self.mpll % freq, 0)
        self.assertEqual(regs["DDRP_CL"], 8)
        self.assertEqual(regs["DDRP_CWL"], 4)

    def test_ginfo_fields(self):
        _, _, _, gi = list(s.parse_tlvs(self.hdr))[0]
        e, cpu, ddr, div, uidx, baud = struct.unpack_from("<6I", gi)
        self.assertEqual((e, ddr, uidx, baud), (24_000_000, 500_000_000, 2, 115200))


class Safety(unittest.TestCase):
    def test_non_divisor_freq_rejected(self):
        self.assertIsNotNone(s.check_ddr_clock(400_000_000, 1_500_000_000))
        self.assertIsNone(s.check_ddr_clock(500_000_000, 1_500_000_000))

    def test_out_of_range_freq(self):
        with self.assertRaises(s.CreatorError):
            s.compute_ddr_regs(s.DdrConfig(freq=600_000_000))   # data rate 1.2 GT/s > table

    def test_layouts_differ_in_enum(self):
        r = s.compute_ddr_regs(s.DdrConfig(freq=500_000_000))
        self.assertEqual(struct.unpack_from("<I", s.pack_ddr(r, "v12"), 36)[0], 3)
        self.assertEqual(struct.unpack_from("<I", s.pack_ddr(r, "cloner"), 36)[0], 2)


if __name__ == "__main__":
    unittest.main()
