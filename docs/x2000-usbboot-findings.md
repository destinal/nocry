# Ingenic X2000(E) USB boot: how the cloner burner works

Last updated 2026-11-25. Everything here comes from static analysis of the blobs, cross-checked against the GPL U-Boot source listed below. Nothing has been observed on hardware yet.

## The blobs ballaswag/ingenic-usbboot ships

| File | SHA-256 | Identity |
|---|---|---|
| `spl.bin` (10360 B) | `6d2b7450a3e518eca602b642c2e67e665214b078b6defc145e6d58f559cab6c7` | `U-Boot SPL 2013.07-00032-ga0524e4 (Aug 24 2023)` |
| `uboot.bin` (411508 B) | `cb53b5195a6cb97c8e66d31e25a03b4e38a096f59e223699eba606acaa5984bc` | `Board: burner_x2000`, Ingenic toolchain Release5.0.2 (2021-09), `bootcmd=burn` |

`uboot.bin` is **not** Creality's U-Boot. It is Ingenic's stock **USB cloner "burner"** (board `burner_x2000`), the stage 2 that `cloner.exe` uploads. It has no `ums`, `fastboot` or `dfu`.

## Source code (GPL-2.0+, U-Boot 2013.07 based)

| Tree | Why it matters |
|---|---|
| [Ingenic-community/uboot-xburst](https://github.com/Ingenic-community/uboot-xburst) @ `1060b51` (2023-02-27) | **Closest match to the shipped blob.** It uses the same 32-bit `union cmd` layout as ballaswag's host code, and the same `medium_type` enum (`EXT_POL=7`, which matches the blob's `EXPY` type 7). It has `board/ingenic/burner_x2000/`. |
| [k1-debian/u-boot](https://github.com/k1-debian/u-boot) @ `271fbe7` (2026-09-28, "Creality K1 Support") | Newer Ingenic `ingenic-master`. It has `board/ingenic/crealityk1/` (the real K1 boot loader config, `x2000_v12`), the eFuse cloner module, and `scboot/jz_sec_v1..v4`. **Its protocol is incompatible**: the cmd structs use 64-bit offsets, and `SEC_KEY` is inserted into the medium enum, so `EXT_POL=8`. It builds with stock Ubuntu `gcc-mipsel-linux-gnu`. |

Where things live:

| What | Where |
|---|---|
| Protocol constants and structs | `include/cloner/cloner.h` |
| USB function and dispatch | `drivers/usb/gadget/f_jz_cloner.c` |
| Gadget glue (`jz_usb_burner_vdr`) | `drivers/usb/gadget/g_burntool.c` |
| Medium back ends | `drivers/usb/gadget/cloner/cloner_mo{u}dle_*.c`, plus `cloner_sfcnor.c`, `cloner_sfcnand.c` and `cloner_spinand_firmware.c` (SPI-NAND SN/MAC/license) |
| Secure boot | `drivers/scboot/` (`otp.c`, `secure.c`) |
| Board config | `board/ingenic/burner_x2000/`, `include/configs/burner_x2000*.h`, `boards.cfg` line `burner_x2000_lpddr2` |

I verified the blob's dispatch table (`0x801566b8`, handler `0x80135418`) case by case against `handle_cmd()` in the uboot-xburst tree.

## Rebuild and comparison with the blob (2026-11-25)

**Recipe:** `tools/burner-build/build.sh`. It uses Docker with Ubuntu 18.04 `gcc-mipsel-linux-gnu` 7.5, the uboot-xburst `1060b51` source, k1-debian's `compiler-gcc7.h`, and `patches/0001-cloner-build-mmc-module-with-sdhci.patch`, and builds the target `burner_x2000_lpddr2`. The output embeds a build date, so hashes differ run to run.

**Comparison tools:**
- `tools/re/simmatch.py` splits the blob into functions at the PIC prologue (`lui gp / addiu gp / addu gp,gp,t9`) and pairs them with our ELF symbols. Pairing is by referenced strings, then by `difflib` ratio on normalized words (immediates and absolute targets masked). It also reports an order-insensitive multiset score, which ignores scheduling differences.
- `tools/re/fpmatch.py` is the earlier exact-fingerprint matcher. It is too strict on its own.

**Results:**
- **ISA.** The blob is plain MIPS32r2: one undecodable word in 300 KB of code. The toolchain banner says "xburst2(fp64)", but both trees pass `-march=mips32r2` in `arch/mips/cpu/xburst2/config.mk`, and that overrides the toolchain default. The remaining codegen differences come from gcc 7.2 (Ingenic) vs 7.5 (Ubuntu): instruction scheduling, the same register allocation.
- **Whole image.** 71% of our code bytes match the blob at a multiset score ≥ 0.98, and 80% at ≥ 0.90.
- **Protocol core (`f_jz_cloner.c`).** 19 of 21 functions score ≥ 0.84 multiset, and most score exactly 1.00: `clmg_*`, `realloc_buf`, `i2c_program`, `register_cloner_moudle`, the bind/set_alt/disable functions. `handle_cmd` (0.87), `f_cloner_setup_handle` (0.95) and `handle_write` (0.94) differ because the blob was built with `CONFIG_JZ_SCBOOT`, which adds the `VR_SEC_*` cases and the `OPS_BURN_*` paths.
- **The blob is an intermediate Ingenic revision (built 2023-09-25),** between the two public trees:

  | Part of the blob | Matches |
  |---|---|
  | Protocol, 32-bit `union cmd`, medium enum (`EXT_POL=7`), all MMC-module strings | uboot-xburst |
  | `board.c` ("XBurst2", multiset 1.00), eFuse cloner module, block-0 DDR patch (`mmc_add_info_to_flash`), SPL (0.81 vs 0.60) | k1-debian |
  | Build config | `CONFIG_JZ_SCBOOT` and eFuse on. Both are off in our build. |
- **Stock uboot-xburst `burner_x2000` has no MMC cloner module.** The Makefile gates it on legacy `CONFIG_JZ_MMC`, but the X2000 uses `CONFIG_JZ_SDHCI`. Our patch fixes that. Without it, the rebuilt burner cannot flash eMMC at all.
- **Blob `clmd_mmc_write`, verified in disassembly.** The block number is the full 64-bit `(partition + offset) >> 9`, truncated to u32, which gives 2 TiB of reach. The count is `(len + 511) >> 9`, and the device index is `ops & 0xffff`.
- **Blob-only block-0 behavior.** When the write is at block 0 and the host has sent a `MAGIC_DDR` block, the burner writes `ddr_type` into the buffer at +0x4480 and recomputes the CRC. **Do not send DDR params when restoring a backup to sector 0**, or the restored image won't be byte-identical.
- **SPL.** The blob's SPL is closer to k1-debian's, with DDR debug prints and a chip "Current Version: V1/V2" check compiled in. Our SPL builds have not been tested on hardware. That test is low-risk, since the SPL only runs from TCSM and a failure just means no re-enumeration.

**Still in our build, and to be patched out for the "safe burner":** `OPS(MEMORY,RAW)` (arbitrary memcpy), `OPS(REGISTER,RAW)` and `OPS(I2C,RAW)` in `handle_write()`. These are unconditional in the source. eFuse and secure boot are already absent, because `CONFIG_JZ_SCBOOT` and `CONFIG_CMD_EFUSE` are off.

## How it works, end to end

1. **Bootrom.** The chip enumerates as `a108:eaef`, and only requests 0x00–0x05 exist at this stage. The host sends `SET_DATA_ADDR 0xb2401000`, `SET_DATA_LEN`, bulk-OUTs `spl.bin`, then sends `PROG_STAGE1 0xb2401800`.
2. **SPL.** It runs in TCSM, sets clocks, UART and DDR from the header blocks, then returns to the bootrom.
3. **Stage 2.** The host sends `SET_DATA_ADDR 0x80100000`, bulk-OUTs `uboot.bin`, then `FLUSH_CACHE` and `PROG_STAGE2 0x80100000`. `bootcmd=burn` registers the "vendor burnner interface" USB function. Each medium module (`MMC\0`=2, `EFUS`=4, `SFC\0`=6, `EXPY`=7) adds itself to a list.
4. **Every burner request** is a vendor control transfer, handled by `f_cloner_setup_handle()`:
   - The setup stage sets `cmd_type = bRequest`. For `GET_ACK` it copies `cloner->ack` (int) into EP0. For `GET_CRC` it copies `ack` then `crc`. For `WRITE` and `UPDATE_CFG` it sets `ack = -EBUSY`.
   - The data stage (wLength bytes, normally the 40-byte `union cmd`) completes into `handle_cmd()`, which switches on `cmd_type`.
5. **`UPDATE_CFG` (0x14).** `cmd.update.length = N`, then N bytes are bulk-OUT into `global_args`. These are TLV `ParameterInfo{magic,size,data[]}` blocks: `DBG\0` (debug_param), `POLI` (policy), and one medium block (`MMC\0`/`SFC\0`/`NAND`).
6. **`INIT` (0x11).** This runs once. `cloner_init()` walks the TLVs and picks out `policy_args` and `debug_args`. It then calls `clmg_init()` on the medium block, for example MMC init with its `mmc_param` (which carries the erase flags).
7. **`WRITE` (0x12).** `cmd.write` gives `{u64 partition; u32 ops; u32 offset; u32 length; u32 crc}`. The buffer is reallocated to `length` and `length` bytes are bulk-OUT. `handle_write()` checks the CRC32 if `debug.transfer_data_chk` is set, then switches on `ops`:
   - `OPS(I2C,RAW)` = `0x30000`: writes I²C registers (PMIC!)
   - `OPS(EFUSE,RAW)` = `0x40000`: `efuse_program()`
   - `OPS(MEMORY,RAW)` = `0x00000`: `memcpy` to an **arbitrary address** (`partition + offset`)
   - `OPS(REGISTER,RAW)` = `0x50000`: writes any register in `0xb0000000–0xb8000000`
   - `OPS_BURN_NKU` `0x40001` / `OPS_BURN_ENUK` `0x40002`: burn secure-boot keys into OTP
   - anything else goes to `clmg_write()`, which dispatches on `ops>>16` (2 = MMC, 6 = SFC, ...) with `ops & 0xffff` as the sub-type (RAW/OOB/IMAGE/...)
8. **`READ` (0x13).** `cmd.read` gives `{u64 partition; u32 ops; u32 offset; u32 length}`. `handle_read()` calls `clmg_read()` (or, for `ops == 1` = `OPS_GET_ENCK`, it dumps the encrypted key). The host then bulk-INs `length` bytes. The CRC is computed only if `transfer_data_chk` is set.
9. **Status.** Poll `GET_ACK` (0x10, 4 bytes) after each operation: 0 means OK and negative means errno (-16 EBUSY, -5 EIO, -22 EINVAL, -38 ENOSYS). `GET_CRC` (0x19) returns `{ack, crc}`.

### Full request map (blob ↔ source)

| bRequest | Name | Effect | Our tool |
|---|---|---|---|
| 0x00–0x05 | bootrom: CPU_INFO, SET_DATA_ADDR/LEN, FLUSH_CACHE, PROG_STAGE1/2 | Load and run code | allow |
| 0x10 | GET_ACK | Read status | allow |
| 0x11 | INIT | Initialize medium from cfg | allow |
| 0x12 | WRITE | See the ops list above | allow **only** `ops>>16 ∈ {2,6}` with safe sub-types |
| 0x13 | READ | Read from medium | allow **only** `ops>>16 ∈ {2,6}` |
| 0x14 | UPDATE_CFG | Upload TLV config | allow (we build the TLVs) |
| 0x15 | SYNC_TIME | `rtc_set()` | not needed |
| 0x16 | REBOOT | `do_reset` | allow |
| 0x17 | POWEROFF | Set reset tag, then `do_reset` | allow |
| 0x18 | CHECK | Medium check | allow |
| 0x19 | GET_CRC | `{ack, crc}` | allow |
| **0x20** | **SEC_INIT** | `init_seboot()` | **forbid** |
| **0x21** | **SEC_BURN_RKCK** | Burns the root/chip key into OTP and sets the protect bit | **forbid, irreversible** |
| 0x23 | SEC_GET_CK_LEN | RSA key length (not in this blob's table) | n/a |
| **0x24** | **SEC_BURN_SECBOOT_EN** | **Burns the secure-boot-enable fuse. After this the bootrom only accepts signed SPLs, which permanently ends unsigned USB boot.** | **forbid, irreversible** |
| 0x25–0x28 | SEC_SEDEN, GET_FLASH_INFO, REBURN, SET_SFC_CS_TOPOLOGY | Newer firmware only | n/a |

**Hard rule:** the transport layer has a single choke point that refuses any request or `ops` value not on the allow-list. Better still, **build our own burner** from source with `CONFIG_JZ_SCBOOT`, `CONFIG_CMD_EFUSE` and the eFuse, I²C, MEMORY and REGISTER write paths compiled out. Then the dangerous operations don't exist on the device at all, and we have no opaque blobs.

## SPL header

The first 0x180 bytes of `spl.bin` are tagged blocks.

The blocks are the same `ParameterInfo{magic,size,data[]}` TLVs that `UPDATE_CFG` uses.

- **`MAGIC_INFO`** (stored little-endian as "FIDB"; size 0xb8; at offset 0x000). Values observed: EXTAL 24 MHz (`0x016e3600`), CPU 800 MHz (`0x2faf0800`), DDR 400 MHz (`0x17d78400`), UART index 2 (matches the K1 debug header), 115200 baud. The struct definition for the fields from +0x20 onwards is in `cloner.h`.
- **`MAGIC_DDR`** (stored as "\0RDD"; size 0xb8; at offset 0x0c0). Parameters for Winbond `W97BV6MK_LPDDR2` at 400 MHz.
- There is no signature block. Signed images use the `SCKEY_*` layout in `cloner.h` (RSA N at 0x300, KU at 0x400, SPL code at 0x800).

## MMC medium (`cloner/cloner_module_mmc.c`, uboot-xburst)

- **Addresses are in bytes.** The block number is `(cmd.partition + cmd.offset) / 512`, where `partition` is u64 and `offset` is u32. So **the u64 `partition` field is the way past 4 GiB.** ballaswag always sends `partition = 0`, which is why it can't reach the end of the disk. For full-disk access, put a 512-aligned base in `partition` and keep `offset` small.
- **`INIT` can wipe the board.** `mmc_param.mmc_erase == 1` (`MMC_ERASE_ALL`) erases the **entire eMMC** during `INIT`. `2` (`MMC_ERASE_PART`) erases up to 10 byte ranges from `mmc_erase_range[]`; `end == -1` means "to the end of the disk". Our tool must always send `mmc_erase = 0`, enforced at the struct-builder level.
- With `debug.write_back_chk` set, the burner reads each block back after writing it. Use that for writes.

## The real K1 boot loader config (`k1-debian/u-boot`)

- `crealityk1_uImage_msc0`: `x2000_v12`, MSC0 eMMC, `GPT_CREATOR`, `SPL_PARAMS_FIXER`. Boot command `boota mmc 0 0x80f00000 6144`.
- `board/ingenic/crealityk1/partitions.tab` is **k1-debian's own** layout (`xboot`, `ota`, `sn_mac` @ 2 MiB/1 MiB, `kernel`, `data`), not Creality stock. The `ota` and `sn_mac` offsets do match stock.

## Bugs in ballaswag `usbboot.c` worth not inheriting

- `cleanup()` closes the device only when the handle **is** NULL (the check is inverted).
- `die("...%x", fname)` passes a string to `%x`.
- `mmc_read` loops forever on a short bulk read, and each retry overwrites the buffer from the start.
- The MB/s calculation divides by `duration`, which can be 0.
- `ack` is never checked after `READ`/`WRITE`, so errors pass silently.
- It never uses the u64 `partition` field, so it can't address anything past 4 GiB.
- Offsets are hardcoded instead of read from the GPT.
- There is no LICENSE file. The source header says GPL-2.0-or-later (Rockbox `jz4760_tools`). The blobs have no stated provenance, but they correspond to the GPL source above.

## Open questions

1. ~~MMC sub-type meaning~~: answered. It is the MMC device index (`find_mmc_device(sub_type)`); 0 = MSC0.
2. ~~Build and compare~~: done (see "Rebuild and comparison"). The exact revision isn't public. The closest public match is uboot-xburst plus the k1-debian additions.
3. Hardware-test our rebuilt SPL and burner on a K1: enumeration, `INIT`, a read of the first 1 MiB, compared against the blob's output.
4. Write `patches/0002` to remove `OPS(MEMORY|REGISTER|I2C, RAW)` from `handle_write()`.
5. Board inventory: USB IDs, SoC, DDR string and GPT for K1, K1 Max, K1C (X2000E), Nebula Pad, Ender-3 V3 KE and K1C 2025 (X2600E).
4. The `sn_mac` partition format on X2000 boards.
