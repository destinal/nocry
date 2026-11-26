#!/usr/bin/env python3
"""
nocry-splhdr: build and decode the parameter header that precedes an Ingenic
X2000(E) burner SPL when it is loaded through the bootrom's USB boot mode.

Image layout (what the bootrom receives at 0xb2401000, then runs at 0xb2401800):

    0x000  TLV  'BDIF' (MAGIC_INFO)  struct global_info   -- clocks, UART, gpio
    ....   TLV  'DDR\\0' (MAGIC_DDR)  struct ddr_reg_value -- precomputed DDRC/PHY regs
    ....   zero terminator (magic == 0)
    0x800  SPL code (u-boot-spl.bin)

TLV = { u32 magic; u32 size; u8 data[size]; }, little endian, packed back to back.
The bootrom itself ignores 0x000-0x7ff; the SPL walks the TLVs (param_manage.c).

DDR register values are computed with a faithful Python port of Ingenic's GPL
host tool tools/ingenic-tools/ddr_creator_x2000/{ddr_params_creator,lpddr2_params}.c
(k1-debian/u-boot @271fbe7, X2000_V12 branch). Golden-tested against that C
code; see tests/test_splhdr.py.

Layouts:
  v12     k1-debian `burner_x2000` (SoC x2000_v12) SPL. ddr_type enum has DDR4
          first, so LPDDR2 == 3. Full 0xd4-byte ddr_reg_value incl. PHY tail.
  cloner  What Ingenic's cloner.exe / ballaswag's spl.bin header uses: older enum
          (LPDDR2 == 2), DDR TLV declared 0xb8 (no PHY tail). For reproducing /
          checking that blob only. Do not pair with the v12 SPL.

Stdlib only. Python >= 3.8.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

HEADER_SIZE = 0x800            # CONFIG_SPL_GINFO_SIZE; SPL code starts here
SPL_MAX_SIZE = 0x5800          # CONFIG_SPL_MAX_SIZE for burner_x2000_v12
LOAD_ADDR = 0xb2401000
EXEC_ADDR = 0xb2401800

def _magic(a, b, c, d) -> int:
    return (ord(a) << 24) | (ord(b) << 16) | (ord(c) << 8) | (ord(d) if d else 0)

MAGIC_INFO = _magic('B', 'D', 'I', 'F')     # stored LE -> "FIDB"
MAGIC_DDR = _magic('D', 'D', 'R', '\0')      # stored LE -> "\0RDD"
MAGIC_DETECT = _magic('D', 'E', 'T', 'E')

# ---------------------------------------------------------------------------
# ddr_type enums. They differ between SPL generations -- this matters: the SPL
# switch()es on h.type to pick the init sequence.
DDR_TYPE_ENUM = {
    "v12":    {"DDR4": 0, "DDR3": 1, "LPDDR": 2, "LPDDR2": 3, "LPDDR3": 4, "DDR2": 5},
    "cloner": {"DDR3": 0, "LPDDR": 1, "LPDDR2": 2, "LPDDR3": 3, "DDR2": 4},
}

LAYOUTS = {
    # ddr_decl_size: value written in the DDR TLV size field
    # ddr_struct_size: bytes we actually emit (struct incl. PHY tail)
    "v12":    {"ddr_decl_size": 0xd4, "ddr_struct_size": 0xd4},
    "cloner": {"ddr_decl_size": 0xb8, "ddr_struct_size": 0xb8},
}

# ---------------------------------------------------------------------------
# DDR chip database. Timings are [min_tCK, ps] => max(min_tCK * tCK, ps), i.e.
# DDR_SELECT_MAX__tCK_ps(). A bare int is picoseconds; {"tck": n} is n * tCK.
# rl_wl: list of [max_data_rate_hz, RL, WL]; first row whose limit >= rate wins.

LPDDR2_W97BV6MK_RLWL = [
    [333_000_000, 3, 1], [400_000_000, 3, 1], [533_000_000, 4, 2],
    [667_000_000, 5, 2], [800_000_000, 6, 3], [933_000_000, 7, 4],
    [1_066_000_000, 8, 4],
]

CHIPS: Dict[str, dict] = {
    # include/ddr/chips-v2/LPDDR2_W97BV6MK.h (k1-debian). X2000E in-package DRAM.
    "W97BV6MK": {
        "type": "LPDDR2", "vendor": 0, "capacity_code": 0,   # WINBOND, MEM_256M
        "row": 14, "row1": 14, "col": 10, "col1": 10, "bank8": 1, "bl": 8,
        "rl_wl": LPDDR2_W97BV6MK_RLWL, "min_data_rate": 50_000_000,
        "tMRW": {"tck": 5}, "tDQSCK": 2000, "tDQSCKMAX": 10000,
        "tRAS": [3, 42000], "tRTP": [2, 7500], "tRP": [3, 21000], "tRCD": [3, 18000],
        "tRRD": [2, 10000], "tWR": [3, 15000], "tWTR": [2, 7500], "tFAW": [8, 50000],
        "tRFC": 130000, "tREFI": 3900000, "tCKE": {"tck": 3},
        "tCKESR": [3, 150000],          # sic: 150 ns in Ingenic's file (JEDEC: 15 ns)
        "tXSR_extra": 10000,            # tXSR = max(2 tCK, tRFC + 10 ns)
        "tXP": [2, 7500],
    },
}
# The chip table baked into Ingenic's closed cloner.exe differs in two timings.
# Inferred from ballaswag's spl.bin header (400 MHz): tWTR <= 5 ns, tCKESR 15 ns.
CHIPS["W97BV6MK_LPDDR2_CLONER"] = dict(CHIPS["W97BV6MK"], tWTR=[2, 0], tCKESR=[3, 15000])


class CreatorError(Exception):
    pass


def _u32(v: int) -> int:
    return v & 0xffffffff


@dataclass
class DdrConfig:
    chip: str = "W97BV6MK"
    name: Optional[str] = None       # h.name (<=31 chars); defaults to chip key
    freq: int = 500_000_000
    cs0: int = 1
    cs1: int = 0
    dw32: int = 0
    odt: bool = False                # CONFIG_DDR_CHIP_ODT
    auto_self_refresh: bool = False  # CONFIG_DDR_AUTO_SELF_REFRESH
    autosr_cnt: int = 257            # CONFIG_DDR_AUTO_SELF_REFRESH_CNT
    actpd: bool = True               # DDRC_CTRL_ACTPD (bit 15); cloner/old-creator style
    mr3_ds: Optional[int] = 2        # LPDDR2 MR3 drive strength (2 = 40 ohm)
    v12: bool = True                 # X2000_V12/M300 controller register layout


@dataclass
class DdrRegs:
    name: str
    id: int
    type_name: str
    freq: int
    regs: Dict[str, int] = field(default_factory=dict)
    remap: List[int] = field(default_factory=list)


REG_ORDER = [
    "DDRC_CFG", "DDRC_CTRL", "DDRC_DLMR", "DDRC_DDLP", "DDRC_MMAP0", "DDRC_MMAP1",
    "DDRC_REFCNT", "DDRC_TIMING1", "DDRC_TIMING2", "DDRC_TIMING3", "DDRC_TIMING4",
    "DDRC_TIMING5", "DDRC_AUTOSR_CNT", "DDRC_AUTOSR_EN", "DDRC_HREGPRO",
    "DDRC_PREGPRO", "DDRC_CGUC0", "DDRC_CGUC1", "DDRP_MEMCFG", "DDRP_CL", "DDRP_CWL",
    "DDR_MR0", "DDR_MR1", "DDR_MR2", "DDR_MR3", "DDR_MR10", "DDR_MR11", "DDR_MR63",
    "DDR_CHIP_0_SIZE", "DDR_CHIP_1_SIZE",
]


def compute_ddr_regs(cfg: DdrConfig, chip: Optional[dict] = None) -> DdrRegs:
    """Port of create_one_ddr_params() for LPDDR2 on X2000."""
    c = chip if chip is not None else CHIPS.get(cfg.chip)
    if c is None:
        raise CreatorError(f"unknown DDR chip {cfg.chip!r}; known: {', '.join(CHIPS)}")
    if c["type"] != "LPDDR2":
        raise CreatorError(f"only LPDDR2 is ported so far (chip type {c['type']})")

    freq = int(cfg.freq)
    pspt = 1_000_000_000 // (freq // 1000)          # __ps_per_tck (C integer math)

    def ceil_c(ps: int, div: int = 1) -> int:       # ps2cycle_ceil()
        return (ps + div * pspt - 1) // pspt

    def t(spec) -> int:                             # chip timing -> ps
        if isinstance(spec, dict):
            return spec["tck"] * pspt
        if isinstance(spec, (list, tuple)):
            tck, ps = spec
            return tck * pspt if tck * pspt > ps else ps
        return int(spec)

    def mask(v: int, bits: int, what: str) -> int:  # ASSERT_MASK(): note C uses BIT_NUM+1
        if v < 0 or v > (1 << (bits + 1)) - 1:
            raise CreatorError(f"{what}={v} does not fit ({bits} bits) at {freq} Hz")
        return v

    # --- chip->init(): RL/WL from data-rate table
    rate = freq * 2
    if rate <= c["min_data_rate"]:
        raise CreatorError(f"data rate {rate} below chip minimum")
    rl = wl = None
    for lim, r, w in c["rl_wl"]:
        if rate <= lim:
            rl, wl = r, w
            break
    if rl is None:
        raise CreatorError(f"data rate {rate} Hz above chip table")

    tRAS, tRP = t(c["tRAS"]), t(c["tRP"])
    tRFC = t(c["tRFC"])
    p = {
        "tRAS": tRAS, "tRP": tRP, "tRCD": t(c["tRCD"]), "tRC": tRAS + tRP,
        "tWR": t(c["tWR"]), "tRRD": t(c["tRRD"]), "tWTR": t(c["tWTR"]),
        "tRFC": tRFC, "tXP": t(c["tXP"]), "tCKE": t(c["tCKE"]), "tREFI": t(c["tREFI"]),
        "tDQSCK": t(c["tDQSCK"]), "tDQSCKMAX": t(c["tDQSCKMAX"]),
        "tCKESR": t(c["tCKESR"]), "tXSR": t([2, tRFC + c["tXSR_extra"]]),
        "tRTP": t(c["tRTP"]), "tFAW": t(c["tFAW"]),
        "tCCD": c["bl"] // 2,                        # in tCK, not ps (sic)
        "RL": rl * pspt, "WL": wl * pspt,            # fill_in_params_lpddr2
    }
    bl = c["bl"]

    # --- init_ddr_params_common / sdram_size
    cs0, cs1, dw32 = cfg.cs0, cfg.cs1, cfg.dw32
    row, col = c["row"], c["col"]
    row1, col1 = (c["row1"], c["col1"]) if cs1 else (0, 0)
    bank8 = c["bank8"]
    banks = 8 if bank8 else 4
    dw = 4 if dw32 else 2
    chip0 = (1 << (row + col)) * dw * banks if cs0 == 1 else 0
    chip1 = (1 << (row1 + col1)) * dw * banks if cs1 == 1 else 0

    # --- MR registers (fill_mr_params_lpddr2)
    nwr = ceil_c(p["tWR"])
    mask(nwr, 3, "nWR")
    if not 3 <= nwr <= 8:
        raise CreatorError(f"tWR={nwr} tCK outside 3..8")
    if bl != 8:
        raise CreatorError("LPDDR2 creator only supports BL8")
    mr1 = (1 << 8) | ((nwr - 2) << 5) | 3            # MA=1, nWR, WC=0, BT=0, BL=log2(8)
    rlc, wlc = ceil_c(p["RL"]), ceil_c(p["WL"])
    rlwl_code = {0x31: 1, 0x42: 2, 0x52: 3, 0x63: 4, 0x74: 5, 0x84: 6}.get(wlc | (rlc << 4))
    if rlwl_code is None:
        raise CreatorError(f"PHY does not support RL={rlc}/WL={wlc}")
    mr2 = (2 << 8) | rlwl_code
    ds = 2 if cfg.mr3_ds is None else (cfg.mr3_ds & 0xf)
    mr3 = (3 << 8) | ds
    mr10 = (0x0a << 8) | 0xff
    mr63 = 0x3f << 8

    # --- ddrc_base_params_creator_common
    t1 = t2 = t3 = t4 = t5 = 0
    t1 |= mask(ceil_c(p["tWR"]), 6, "tWR") << 8
    t1 |= mask(ceil_c(p["WL"]), 6, "tWL")
    t2 |= mask(ceil_c(p["RL"]), 6, "tRL")
    t3 |= mask(ceil_c(p["tRCD"]), 6, "tRCD") << 16
    t3 |= mask(ceil_c(p["tRP"]), 6, "tRP")
    t3 |= 3 << 24                                     # tEXTRW
    t4 |= mask(ceil_c(p["tRAS"]), 6, "tRAS") << 8
    t4 |= mask(ceil_c(p["tRRD"]), 6, "tRRD")
    trc = ceil_c(p["tRC"])
    t4 |= mask(trc, 7 if cfg.v12 else 6, "tRC") << 16
    tcke = mask(ceil_c(p["tCKE"]), 4 if cfg.v12 else 3, "tCKE")
    t5 |= tcke & (0xf if cfg.v12 else 0x7)
    txp = ceil_c(p["tXP"]) or 1
    t5 |= mask(txp, 4, "tXP") << 4

    # get_refcnt_value
    reftck = p["tREFI"] // pspt
    if not cfg.v12:
        reftck -= 16
    factor = 0
    while True:
        con = reftck // (16 << factor)
        factor += 1
        if con <= 255:
            break
    div = factor - 1
    if div > 7:
        raise CreatorError("tREFI too large")
    mask(con, 8, "refcnt.con")
    rfc = ceil_c(p["tRFC"], 2) // 2
    mask(rfc, 8 if cfg.v12 else 6, "tRFC")
    refcnt = (con << 16) | (div << 1) | 1 | (1 << 30) | (8 << 4)
    if not cfg.v12:
        refcnt |= rfc << 24
    autosr_cnt = ((rfc << 24) | cfg.autosr_cnt) if cfg.v12 else cfg.autosr_cnt
    autosr_en = 1 if cfg.auto_self_refresh else 0

    # --- ddrc_params_creator_lpddr2
    t1 |= mask(ceil_c(p["WL"]), 6, "tWDLAT") << 24
    t1 |= mask(ceil_c(p["WL"] + p["tWTR"]) + p["tCCD"] + 1, 6, "tWTR") << 16
    t2 |= mask(ceil_c(p["RL"]) - 2, 6, "tRDLAT") << 24
    t2 |= mask(ceil_c(p["RL"] + p["tDQSCKMAX"] - p["WL"]) + p["tCCD"] + 1, 6, "tRTW") << 16
    t2 |= mask(ceil_c(p["tRTP"]), 6, "tRTP") << 8
    t3 |= (p["tCCD"] & 0x3f) << 8
    t4 |= (mask(ceil_c(p["tFAW"]), 6, "tFAW") & 0xff) << 24
    t5 |= (mask(ceil_c(p["tXSR"], 4) // 4, 8, "tXS") & 0xff) << 24
    t5 |= (mask(ceil_c(p["tCKESR"]), 8, "tCKESR") & 0xff) << 16
    t5 |= 2 << 12                                     # tCKSRE

    # --- ddrc_config_creator
    cfgreg = (cs0 & 1) | ((cs1 & 1) << 1) | ((1 if cfg.odt else 0) << 2) | (5 << 3)  # TYPE LPDDR2=0b101
    cfgreg |= (bank8 & 1) << 9
    cfgreg |= ((col - 8) & 7) << 10
    cfgreg |= ((row - 12) & 7) << 13
    cfgreg |= (bank8 & 1) << 25
    cfgreg |= ((col1 - 8) & 7) << 26                  # C bitfield truncation of negatives
    cfgreg |= ((row1 - 12) & 7) << 29
    ctrl = (3 << 12) | (1 << 1) | (1 << 7) | (1 << 4)  # PDT_32|CKE|PD_CCE|SR_CCE
    if cfg.actpd:
        ctrl |= 1 << 15

    memsize = chip0 + chip1
    MMASK = 0xff
    if memsize > 0x20000000:
        if chip1:
            base0 = 0
            mask0 = (~((chip0 >> 24) - 1) & ~(memsize >> 24)) & MMASK
            base1 = (chip1 >> 24) & 0xff
            mask1 = (~((chip1 >> 24) - 1) & ~(memsize >> 24)) & MMASK
        else:
            base0, mask0 = 0, ~(((chip0 * 2) >> 24) - 1) & MMASK
            mask1, base1 = 0, 0xff
    else:
        base0 = (0x20000000 >> 24) & 0xff
        mask0 = ~((chip0 >> 24) - 1) & MMASK
        base1 = ((0x20000000 + chip0) >> 24) & 0xff
        mask1 = ~((chip1 >> 24) - 1) & MMASK
    mmap0 = (base0 << 8) | mask0
    mmap1 = (base1 << 8) | mask1

    # --- ddrp: lpddr2 + config
    cwl = mask(ceil_c(p["WL"]), 4, "CWL")
    cl = mask(ceil_c(p["RL"]), 8, "CL")
    memcfg = 3 | (1 << 4)                             # memsel LPDDR2=3, brusel BL8

    # --- fill_mem_remap (non-X2600 branch)
    remap = bytearray(range(20))
    if not (chip1 and chip0 != chip1):
        bank_bits = 3 if bank8 == 1 else 2
        bit_width = 2 if dw32 == 1 else 1
        address_bits = bit_width + col + row + bank_bits + (cs0 + cs1 - 1)
        swap_bits = bank_bits + (cs0 + cs1 - 1)
        start_a = bit_width + col if bit_width + col > 12 else 12
        start_b = address_bits - swap_bits - start_a
        start_a -= 12
        for i in range(start_a, swap_bits):
            a, b = start_a + i, start_b + i
            remap[a], remap[b] = remap[b], remap[a]
    remap_words = list(struct.unpack("<5I", bytes(remap)))

    regs = {
        "DDRC_CFG": cfgreg, "DDRC_CTRL": ctrl, "DDRC_DLMR": 0, "DDRC_DDLP": 0,
        "DDRC_MMAP0": mmap0, "DDRC_MMAP1": mmap1, "DDRC_REFCNT": refcnt,
        "DDRC_TIMING1": t1, "DDRC_TIMING2": t2, "DDRC_TIMING3": t3,
        "DDRC_TIMING4": t4, "DDRC_TIMING5": t5,
        "DDRC_AUTOSR_CNT": autosr_cnt, "DDRC_AUTOSR_EN": autosr_en,
        "DDRC_HREGPRO": 1, "DDRC_PREGPRO": 1,
        "DDRC_CGUC0": 0x11111111, "DDRC_CGUC1": (1 << 8) | (1 << 4) | (1 << 1) | 1,
        "DDRP_MEMCFG": memcfg, "DDRP_CL": cl, "DDRP_CWL": cwl,
        "DDR_MR0": 0, "DDR_MR1": mr1, "DDR_MR2": mr2, "DDR_MR3": mr3,
        "DDR_MR10": mr10, "DDR_MR11": 0, "DDR_MR63": mr63,
        "DDR_CHIP_0_SIZE": chip0, "DDR_CHIP_1_SIZE": chip1,
    }
    regs = {k: _u32(v) for k, v in regs.items()}
    chip_id = (0 << 6) | (c["vendor"] << 3) | c["capacity_code"]   # DDR_CHIP_ID(vendor, TYPE_LPDDR2=0, cap)
    return DdrRegs(name=(cfg.name or cfg.chip), id=chip_id, type_name=c["type"],
                   freq=freq, regs=regs, remap=remap_words)


# ---------------------------------------------------------------------------
# Packing

def pack_ddr(d: DdrRegs, layout: str) -> bytes:
    lay = LAYOUTS[layout]
    name = d.name.encode("ascii")
    if len(name) > 31:
        raise CreatorError("DDR name longer than 31 bytes")
    out = bytearray(name.ljust(32, b"\0"))
    out += struct.pack("<3I", d.id, DDR_TYPE_ENUM[layout][d.type_name], d.freq)
    out += struct.pack("<30I", *(d.regs[k] for k in REG_ORDER))
    out += struct.pack("<5I", *d.remap)
    # PHY drive/ODT (13 B) + deskew (14 B) + pad: zero. The burner SPL memsets
    # them anyway (get_ddr_params_burner).
    out += bytes(lay["ddr_struct_size"] - len(out))
    assert len(out) == lay["ddr_struct_size"]
    return bytes(out)


@dataclass
class BoardInfo:
    extal: int = 24_000_000
    cpufreq: int = 800_000_000
    ddrfreq: int = 0                  # filled from DDR config if 0
    ddr_div: int = 0
    uart_idx: int = 2
    baud: int = 115200
    uart_gpio: Tuple[int, int, int] = (3, 0, 0xc0000000)    # PD30/31 = UART2
    gpio_funcs: List[Tuple[int, int, int]] = field(default_factory=list)
    size: int = 0x94                  # sizeof(struct global_info) in the v12 SPL


def pack_ginfo(b: BoardInfo) -> bytes:
    out = bytearray(struct.pack("<6I", b.extal, b.cpufreq, b.ddrfreq, b.ddr_div, b.uart_idx, b.baud))
    out += struct.pack("<3I", *b.uart_gpio)
    out += struct.pack("<I", len(b.gpio_funcs))
    for g in b.gpio_funcs:
        out += struct.pack("<3I", *g)
    if len(out) > b.size:
        raise CreatorError(f"global_info content {len(out):#x} exceeds size {b.size:#x}")
    out += bytes(b.size - len(out))
    return bytes(out)


def tlv(magic: int, payload: bytes, decl_size: Optional[int] = None) -> bytes:
    size = len(payload) if decl_size is None else decl_size
    return struct.pack("<2I", magic, size) + payload


def build_header(board: BoardInfo, ddr: DdrRegs, layout: str) -> bytes:
    if layout not in LAYOUTS:
        raise CreatorError(f"unknown layout {layout!r}")
    if board.ddrfreq == 0:
        board.ddrfreq = ddr.freq
    hdr = tlv(MAGIC_INFO, pack_ginfo(board))
    hdr += tlv(MAGIC_DDR, pack_ddr(ddr, layout), LAYOUTS[layout]["ddr_decl_size"])
    hdr += bytes(8)                                    # terminator TLV (magic 0)
    if len(hdr) > HEADER_SIZE:
        raise CreatorError(f"header {len(hdr):#x} > {HEADER_SIZE:#x}")
    return hdr + bytes(HEADER_SIZE - len(hdr))


# ---------------------------------------------------------------------------
# Profiles

def _int(v) -> int:
    return int(v, 0) if isinstance(v, str) else int(v)


def load_profile(path: str) -> dict:
    with open(path) as f:
        return json.load(f)


def profile_to_objects(prof: dict, ddr_freq: Optional[int] = None):
    d = prof.get("ddr", {})
    cfg = DdrConfig(
        chip=d.get("chip", "W97BV6MK"), name=d.get("name"),
        freq=_int(ddr_freq if ddr_freq is not None else d.get("freq", 500_000_000)),
        cs0=d.get("cs0", 1), cs1=d.get("cs1", 0), dw32=d.get("dw32", 0),
        odt=d.get("odt", False), auto_self_refresh=d.get("auto_self_refresh", False),
        autosr_cnt=_int(d.get("autosr_cnt", 257)), actpd=d.get("actpd", True),
        mr3_ds=d.get("mr3_ds", 2), v12=d.get("v12", True),
    )
    chip = None
    if "chip_timings" in d:                      # full custom chip definition
        chip = d["chip_timings"]
    u = prof.get("uart", {})
    g = u.get("gpio", {"port": 3, "func": 0, "pins": "0xc0000000"})
    board = BoardInfo(
        extal=_int(prof.get("extal", 24_000_000)), cpufreq=_int(prof.get("cpufreq", 800_000_000)),
        ddrfreq=_int(prof.get("ddrfreq", 0)), ddr_div=_int(prof.get("ddr_div", 0)),
        uart_idx=_int(u.get("idx", 2)), baud=_int(u.get("baud", 115200)),
        uart_gpio=(_int(g["port"]), _int(g["func"]), _int(g["pins"])),
        gpio_funcs=[(_int(x["port"]), _int(x["func"]), _int(x["pins"])) for x in prof.get("gpio_funcs", [])],
        size=_int(prof.get("ginfo_size", 0x94)),
    )
    layout = prof.get("layout", "v12")
    mpll = _int(prof["mpll"]) if "mpll" in prof else None
    return cfg, chip, board, layout, mpll


def check_ddr_clock(freq: int, mpll: Optional[int]) -> Optional[str]:
    if mpll is None:
        return None
    if mpll % freq:
        return (f"DDR freq {freq} Hz does not divide the SPL's MPLL {mpll} Hz; "
                f"clk_set_rate() will round the divider and the timings will not match "
                f"(valid: {', '.join(str(mpll // n) for n in range(2, 9) if mpll % n == 0)})")
    return None


# ---------------------------------------------------------------------------
# Decoding

def parse_tlvs(buf: bytes, limit: int = HEADER_SIZE):
    off = 0
    while off + 8 <= min(len(buf), limit):
        magic, size = struct.unpack_from("<2I", buf, off)
        if magic == 0:
            return
        if magic not in (MAGIC_INFO, MAGIC_DDR, MAGIC_DETECT) or off + 8 + size > limit:
            yield off, magic, size, None          # unknown / garbage: SPL stops here
            return
        yield off, magic, size, buf[off + 8: off + 8 + size]
        off += 8 + size


def decode_timings(regs: Dict[str, int], freq: int) -> Dict[str, int]:
    f = lambda r, lo, w: (regs[r] >> lo) & ((1 << w) - 1)
    return {
        "tWL": f("DDRC_TIMING1", 0, 6), "tWR": f("DDRC_TIMING1", 8, 6),
        "tWTR": f("DDRC_TIMING1", 16, 6), "tWDLAT": f("DDRC_TIMING1", 24, 6),
        "tRL": f("DDRC_TIMING2", 0, 6), "tRTP": f("DDRC_TIMING2", 8, 6),
        "tRTW": f("DDRC_TIMING2", 16, 6), "tRDLAT": f("DDRC_TIMING2", 24, 6),
        "tRP": f("DDRC_TIMING3", 0, 6), "tCCD": f("DDRC_TIMING3", 8, 6),
        "tRCD": f("DDRC_TIMING3", 16, 6), "tEXTRW": f("DDRC_TIMING3", 24, 3),
        "tRRD": f("DDRC_TIMING4", 0, 6), "tRAS": f("DDRC_TIMING4", 8, 6),
        "tRC": f("DDRC_TIMING4", 16, 7), "tFAW": f("DDRC_TIMING4", 24, 8),
        "tCKE": f("DDRC_TIMING5", 0, 4), "tXP": f("DDRC_TIMING5", 4, 4),
        "tCKSRE": f("DDRC_TIMING5", 12, 4), "tCKESR": f("DDRC_TIMING5", 16, 8),
        "tXS(x4)": f("DDRC_TIMING5", 24, 8),
        "refcnt.con": f("DDRC_REFCNT", 16, 8), "refcnt.div": f("DDRC_REFCNT", 1, 3),
        "tRFC/2 (autosr_cnt[31:24])": f("DDRC_AUTOSR_CNT", 24, 8),
    }


def decode_ddr(payload: bytes):
    name = payload[:32].split(b"\0", 1)[0].decode("ascii", "replace")
    did, dtype, freq = struct.unpack_from("<3I", payload, 32)
    vals = struct.unpack_from("<30I", payload, 44)
    regs = dict(zip(REG_ORDER, vals))
    remap = list(struct.unpack_from("<5I", payload, 44 + 120))
    return name, did, dtype, freq, regs, remap


def decode_image(buf: bytes) -> str:
    lines = []
    for off, magic, size, payload in parse_tlvs(buf):
        tag = struct.pack("<I", magic)
        if payload is None:
            lines.append(f"{off:#05x}: unparsed data (magic {magic:#010x} {tag!r}, size {size:#x}) -- SPL stops here")
            continue
        lines.append(f"{off:#05x}: TLV {tag!r} magic={magic:#010x} size={size:#x}")
        if magic == MAGIC_INFO:
            e, cpu, ddr, ddiv, uidx, baud = struct.unpack_from("<6I", payload, 0)
            up, uf, upins, ngpio = struct.unpack_from("<4I", payload, 0x18)
            lines.append(f"    extal={e} cpufreq={cpu} ddrfreq={ddr} ddr_div={ddiv} uart_idx={uidx} baud={baud}")
            lines.append(f"    uart_gpio: port={up} func={uf} pins={upins:#010x}; nr_gpio_func={ngpio}")
            for i in range(min(ngpio, 8)):
                gp, gf, gpins = struct.unpack_from("<3I", payload, 0x28 + 12 * i)
                lines.append(f"    gpio[{i}]: port={gp} func={gf} pins={gpins:#010x}")
        elif magic == MAGIC_DDR:
            if size >= 0xb8 and payload[:1].isalnum():
                name, did, dtype, freq, regs, remap = decode_ddr(payload)
                tnames = {v: k for k, v in DDR_TYPE_ENUM["v12"].items()}
                cnames = {v: k for k, v in DDR_TYPE_ENUM["cloner"].items()}
                lines.append(f"    name={name!r} id={did:#x} type={dtype} (v12 enum: {tnames.get(dtype,'?')}, "
                             f"cloner enum: {cnames.get(dtype,'?')}) freq={freq}")
                for k in REG_ORDER:
                    lines.append(f"    {k:<16} = {regs[k]:#010x}")
                lines.append("    REMMAP = " + " ".join(f"{w:#010x}" for w in remap))
                if freq:
                    td = decode_timings(regs, freq)
                    lines.append("    timings (tCK): " + ", ".join(f"{k}={v}" for k, v in td.items()))
            else:
                lines.append("    (headerless struct ddr_registers -- legacy uboot-xburst SPL format)")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# CLI

def cmd_build(a) -> int:
    prof = load_profile(a.profile)
    cfg, chip, board, layout, mpll = profile_to_objects(prof, a.ddr_freq)
    if a.layout:
        layout = a.layout
    warn = check_ddr_clock(cfg.freq, mpll)
    if warn and not a.force:
        print(f"error: {warn}\n(use --force to override)", file=sys.stderr)
        return 2
    elif warn:
        print(f"warning: {warn}", file=sys.stderr)
    regs = compute_ddr_regs(cfg, chip)
    hdr = build_header(board, regs, layout)
    if a.header_only:
        data = hdr
    else:
        if not a.spl:
            print("error: --spl is required unless --header-only", file=sys.stderr)
            return 2
        with open(a.spl, "rb") as f:
            code = f.read()
        if code[:4] in (b"FIDB", b"\0RDD"):
            print("error: --spl already has a parameter header; pass the raw u-boot-spl.bin", file=sys.stderr)
            return 2
        if len(code) > SPL_MAX_SIZE:
            print(f"error: SPL is {len(code)} bytes > CONFIG_SPL_MAX_SIZE {SPL_MAX_SIZE}", file=sys.stderr)
            return 2
        data = hdr + code
    with open(a.output, "wb") as f:
        f.write(data)
    print(f"wrote {a.output}: {len(data)} bytes (layout={layout}, DDR {regs.name} @ {cfg.freq/1e6:g} MHz, "
          f"load {LOAD_ADDR:#x}, exec {EXEC_ADDR:#x})")
    return 0


def cmd_decode(a) -> int:
    with open(a.image, "rb") as f:
        buf = f.read()
    print(decode_image(buf))
    if len(buf) > HEADER_SIZE:
        print(f"SPL code: {len(buf) - HEADER_SIZE} bytes at file offset {HEADER_SIZE:#x} (exec {EXEC_ADDR:#x})")
    return 0


def cmd_regs(a) -> int:
    prof = load_profile(a.profile)
    cfg, chip, board, layout, mpll = profile_to_objects(prof, a.ddr_freq)
    r = compute_ddr_regs(cfg, chip)
    print(f"{r.name} {r.type_name} @ {r.freq} Hz (id={r.id:#x})")
    for k in REG_ORDER:
        print(f"  {k:<16} = {r.regs[k]:#010x}")
    print("  REMMAP = " + " ".join(f"{w:#010x}" for w in r.remap))
    print("  timings (tCK): " + ", ".join(f"{k}={v}" for k, v in decode_timings(r.regs, r.freq).items()))
    w = check_ddr_clock(cfg.freq, mpll)
    if w:
        print(f"warning: {w}")
    return 0


def cmd_compare(a) -> int:
    bufs = []
    for p in (a.a, a.b):
        with open(p, "rb") as f:
            bufs.append(f.read()[:HEADER_SIZE])
    diffs = [i for i in range(HEADER_SIZE) if bufs[0][i:i+1] != bufs[1][i:i+1]]
    if not diffs:
        print("headers identical")
        return 0
    runs, start, prev = [], diffs[0], diffs[0]
    for i in diffs[1:]:
        if i != prev + 1:
            runs.append((start, prev)); start = i
        prev = i
    runs.append((start, prev))
    for s, e in runs:
        print(f"differ {s:#05x}-{e:#05x}: {bufs[0][s:e+1].hex()} vs {bufs[1][s:e+1].hex()}")
    return 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="nocry-splhdr", description=__doc__.split("\n\n")[0])
    sp = ap.add_subparsers(dest="cmd", required=True)
    b = sp.add_parser("build", help="build header (+SPL) from a board profile")
    b.add_argument("--profile", required=True)
    b.add_argument("--spl", help="raw u-boot-spl.bin (code only, runs at 0xb2401800)")
    b.add_argument("-o", "--output", required=True)
    b.add_argument("--header-only", action="store_true")
    b.add_argument("--ddr-freq", type=lambda s: int(s, 0))
    b.add_argument("--layout", choices=sorted(LAYOUTS))
    b.add_argument("--force", action="store_true", help="allow DDR freq that does not divide MPLL")
    b.set_defaults(fn=cmd_build)
    d = sp.add_parser("decode", help="decode the header of an spl image")
    d.add_argument("image")
    d.set_defaults(fn=cmd_decode)
    r = sp.add_parser("regs", help="print computed DDR registers for a profile")
    r.add_argument("--profile", required=True)
    r.add_argument("--ddr-freq", type=lambda s: int(s, 0))
    r.set_defaults(fn=cmd_regs)
    c = sp.add_parser("compare", help="byte-compare the 0x800-byte headers of two images")
    c.add_argument("a"); c.add_argument("b")
    c.set_defaults(fn=cmd_compare)
    a = ap.parse_args(argv)
    try:
        return a.fn(a)
    except CreatorError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
