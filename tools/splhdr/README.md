# nocry-splhdr

`nocry-splhdr` builds and decodes the 0x800-byte parameter header that precedes an Ingenic X2000(E) burner SPL in USB boot. It is a single stdlib-only Python file (3.8+).

```
file offset  TCSM address
0x000        0xb2401000   TLV 'BDIF'  struct global_info   (clocks, UART, gpio)
0x09c        0xb240109c   TLV 'DDR\0' struct ddr_reg_value (DDRC/PHY register values)
0x178        ...          terminator (magic 0)
0x800        0xb2401800   SPL code; the bootrom jumps here (PROG_STAGE1)
```

The bootrom ignores the header. The SPL walks the TLVs (`param_manage.c`) and programs the DDR controller and PHY directly from the register block. **The DDR clock is also taken from the header:** `clk_set_rate(DDR, h.freq)` picks the nearest divider of the compiled-in MPLL.

## Usage

```sh
# Full stage-1 image for usbboot --stage1
./nocry_splhdr.py build --profile profiles/creality-x2000e.json \
    --spl ../burner-build/out/spl-x2000e/u-boot-spl.bin -o spl-x2000e.bin

./nocry_splhdr.py decode spl.bin                 # any image, including ballaswag's/cloner's
./nocry_splhdr.py regs --profile profiles/creality-x2000e.json --ddr-freq 375000000
./nocry_splhdr.py compare a.bin b.bin            # byte diff of the two headers
```

`tools/burner-build/build.sh` builds the k1-debian `burner_x2000` SPL and runs this tool, which produces `out/spl-x2000e/spl-x2000e.bin`.

## Where the numbers come from

- **Struct layouts and the `ddr_type` enum** are read from the DWARF of the SPL we build: k1-debian `burner_x2000`, SoC `x2000_v12`. `global_info` is 0x94 bytes and `ddr_reg_value` is 0xd4 bytes, with **LPDDR2 = 3**.
- **DDR registers** come from a Python port of Ingenic's GPL C creator (`tools/ingenic-tools/ddr_creator_x2000/{ddr_params_creator,lpddr2_params}.c`, X2000_V12 branch) together with the chip timing table (`include/ddr/chips-v2/LPDDR2_W97BV6MK.h`).
- **The golden test** (`tests/golden_c_creator_w97bv6mk.json`) is the C creator's own output at 300, 375 and 500 MHz, and the port matches it bit for bit.
- **The cloner blob is reproduced.** With `profiles/ingenic-cloner-x2000e-400mhz.json`, the generated header equals bytes `0x000–0x17f` of ballaswag's `spl.bin` exactly. Bytes from 0x180 onward are never parsed by the SPL. Run that check with `NOCRY_BLOB_SPL=…/spl.bin python3 -m unittest discover -s tests`.

## Things that will bite you

| Pitfall | Why it matters |
|---|---|
| **The `ddr_type` enum changed between SPL generations** | The cloner's header says `type=2` (LPDDR2 in its enum). The v12 SPL reads 2 as **LPDDR**, which is the wrong init sequence. Never pair the cloner/blob header with the v12 SPL, or the reverse. `decode` prints both interpretations. |
| **Timings are per frequency** | The blob's header carries 400 MHz timings. The v12 SPL's MPLL is 1500 MHz, so the valid DDR clocks are 750, 500, 375, 300, 250 MHz and so on. `build` refuses frequencies that don't divide the MPLL. |
| **Ingenic's cloner used a different chip table** | Its table had tWTR ≤ 5 ns and tCKESR 15 ns, where the open table has 7.5 ns and 150 ns (JEDEC: max(2tCK, 7.5 ns) and max(3tCK, 15 ns)). It also had ACTPD on, and ODT and auto-self-refresh off. The K1 profile uses the open timings, which are what k1-debian runs at 500 MHz, plus the cloner's ACTPD/ODT/ASR settings. |
| **The uboot-xburst SPL uses a different, headerless DDR struct** | That format is `struct ddr_registers`. It isn't supported, because that SPL is a non-V12 build and the K1 silicon follows V12 register semantics. |

## Profiles

| Profile | Use |
|---|---|
| `creality-x2000e.json` | X2000E (V12) with in-package W97BV6MK at 500 MHz, UART2 at 115200. K1 and K1 Max. K1C (pre-2025) and Nebula Pad are expected to match but are **unverified**. |
| `ingenic-cloner-x2000e-400mhz.json` | Reproduces the cloner/blob header. For verification only. |

## Not yet supported

LPDDR3, DDR2 and DDR3 creators (the X2000 non-E uses LPDDR3), and X2600 (K1C 2025, which is secure-boot anyway).
