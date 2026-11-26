# nocry-ums: USB mass-storage recovery stage 2 for X2000(E)

Status (2026-11-25): **Test A works on hardware.** Board: Creality Nebula Pad (variant not identified), bootrom reports cpu info `"X2000"`. ballaswag's `spl.bin` plus `nocry_ums_ro_x2000`, loaded by `host/` `nocry boot`. Results:

- The SPL returned in 0.30 s, and the gadget enumerated as `1209:0001`.
- The eMMC appeared as `/dev/sda`: 7.28 GiB, 15273600 sectors, `ro=1`, model "nocry eMMC".
- The GPT matches the K1 layout: `ota`@1M, `sn_mac`@2M, `rtos`/`rtos2`, `kernel`/`kernel2`, `rootfs` (squashfs)/`rootfs2`, `rootfs_data` (ext2), `userdata` (ext2, 6 GiB).

Test B (our own SPL) has not been run yet.

## V12 stage2 and performance (2026-11-25)

The stage2 is now built from k1-debian `x2000_v12` (`tools/burner-build/patches/k1deb/0001-*`), targets `nocry_ums_x2000_v12` and `nocry_ums_ro_x2000_v12`. The uboot-xburst `x2000` target used non-V12 pin tables (MSC0 on PF, UART2 on PA) and is now legacy-only.

| Change | Result |
|---|---|
| MSC0 clock 24 → 52 MHz (Ingenic's V12 burner value) | reads **3.1 → 11.0–11.3 MB/s** |
| `FSG_BUFLEN` 16 → 128 KiB, matched by host `max_sectors=256` via udev | |
| SDMA | **Off.** With it on, reads returned stale or garbled 32/64-byte lines. Post-DMA L1+L2 and L1-only invalidates both made it worse, which means data arrives via the PIO path even in SDMA mode. |

Validated:
- Four 64 MiB rereads: 0 differences.
- All 1027 MiB of static regions identical to the PIO-made backup.
- Two independent full-disk reads bit-identical (sha256 `aee30689…`, now saved as `backups/nebula-full-2.img`).
- The data partitions differ from the first backup only because the board booted Linux in between (new ext2 mount times; e2fsck clean).

Read-write (`nocry_ums_x2000_v12`), on hardware:
- 256 MiB random write into the empty `rootfs2` at **8.4 MiB/s**, verified twice with 0 differences, then zeros restored and verified.
- `nocry snmac --set-model` (identical record) written and verified.
- Full-disk sha256 afterwards = `nebula-full-2.img` (`aee30689…`): the device is bit-identical.

Hold BOOT when power-cycling, or the printer OS boots and changes `rootfs_data`/`userdata`.

## Test B: own SPL, no Ingenic blobs (2026-11-25)

Stage1 `tools/burner-build/out/spl-x2000e/spl-x2000e.bin` is the k1-debian `nocry_spl_x2000_v12` SPL plus a `tools/splhdr` header (W97BV6MK LPDDR2 @ **500 MHz**, MPLL 1500/3). With the V12 read-only stage2:

| Check | Result |
|---|---|
| SPL returned | 0.30 s |
| Four 64 MiB rereads | 0 differences, 10.4 MiB/s |
| Static regions | all 1027 MiB identical to `nebula-full-2.img` |
| Full-disk sha256 | `aee30689…`, **identical to `nebula-full-2.img`** (7.8 GB through 500 MHz DDR, 10.6 MB/s) |

Why `nocry_spl_x2000_v12` and not the stock `burner_x2000` SPL: stock V12 burners define `CONFIG_SPL_USB_BOOT`, which runs an SPL-side USB loop *before* DDR init. That loop answers GET_CPU_INFO itself (with the SoC-ID chip name), takes SET_DATA_ADDR/LEN and PROG_START2, and only proceeds to DDR init after `EP0_EXIT_LOOP` (request 0x08); a DETE header TLV makes it a detect-only run. That is Ingenic's Cloner auto-detection. `nocry boot` would mistake the SPL's loop for the returned bootrom and load stage2 into uninitialised DDR. `NOCRY_SPL` drops that loop, so the SPL behaves like the cloner-era `spl.bin`. Supporting newer stock SPLs (for example the one in `.ingenic` packages) needs the host to send EXIT_LOOP: TODO.

## What it is

This is a replacement for Ingenic's burner as stage 2 of USB boot. Instead of speaking the cloner vendor protocol, it exports the **whole eMMC user area (MSC0)** as a standard USB mass-storage disk.

- It runs only from RAM and writes nothing to flash by itself. Power-cycling the board returns it to normal.
- It contains no cloner protocol code and no eFuse or secure-boot code; the symbol count for all of these is 0.
- Two builds:

| Target | Behavior |
|---|---|
| `nocry_ums_ro_x2000` | Read-only. The LUN reports write-protected (`curlun->ro = 1`), and `ums_write_sector()` returns -1 as a second guard. **Use this for first tests and backups.** |
| `nocry_ums_x2000` | Read-write. For restore and editing, such as `sn_mac`. |

- USB ID: `1209:0001` (pid.codes test PID, manufacturer "nocry"). **This is a placeholder; register a real pid.codes PID before release.**

## Build

```sh
tools/burner-build/build.sh                        # burner + both ums variants -> tools/burner-build/out/<target>/
tools/burner-build/build.sh OUT nocry_ums_ro_x2000 # just one
```

The base is uboot-xburst `1060b51` plus the patches in `tools/burner-build/patches/`:

- `0001`: build the cloner MMC module when the board uses SDHCI. This only affects the burner.
- `0002`: the `nocry_ums_x2000` and `nocry_ums_ro_x2000` targets. Changes by file:

| File | Change |
|---|---|
| `boards.cfg` | Two new targets that reuse the `burner_x2000` board and config, without CMD_BURN, SFC or NAND |
| `include/configs/burner_x2000.h` | `CONFIG_NOCRY_UMS` block: bootcmd `ums 0`, `USBDOWNLOAD_GADGET`, `CMD_USB_MASS_STORAGE`, VID/PID. It keeps `CONFIG_BURNER`, which here only means "take DDR, clock and UART parameters from the SPL header at runtime" (`param_manage.c`). |
| `board/ingenic/burner_x2000/board.c` | `board_ums_init()` (runs `mmc_init` and reports capacity), sector read/write and capacity callbacks, the board string, and a guarded `board_nand_init` stub |
| `drivers/usb/gadget/jz47xx_dwc2_udc.c` | Under `NOCRY_UMS`, a full UDC re-init: set DCTL.SftDiscon, wait 500 ms (so the host sees the bootrom's `a108:eaef` device unplug), PHY init, core reset, device init, then clear SftDiscon. The burner path keeps the bootrom's enumeration, which a class-driver gadget can't use. |
| `drivers/usb/gadget/storage_common.c` | Force `ro = 1` under `NOCRY_UMS_RO` |
| `drivers/usb/gadget/f_mass_storage.c` | Port fixes: skip the ARM `set_bit`/`clear_bit` on MIPS, and use `config.c`'s `usb_copy_descriptors` |
| `drivers/usb/gadget/g_dnl.c` | Guard `dfu_add()` and `fsg_add()` with their configs |

### Static verification

- The entry stub matches the blob, with `_start` at `0x80100000`. Sizes are about 177 KB, compared with 375 KB for the burner.
- The disassembly shows, in order: DCTL `|= 2`, then `mdelay`, then `otg_phy_init(DEVICE_ONLY, 24 MHz)`, then core reset and device init, then DCTL `&= ~2`.
- RO build: `ums_write_sector` is `jr ra; li v0,-1`. In `fsg_add`, the `ro` bitfield is set from the constant 1; the RW build uses `initially_ro`.
- Stage 2 has no `sdram_init`, so DDR is initialized only by the SPL. Stage 2 reads only `extal`, `uart_idx` and `baud_rate` from `global_info`, at offsets 0x00, 0x10 and 0x14. These match the FIDB block in the blob's `spl.bin`. The blob's block is 0xb8 bytes against our struct's 0x90; the extra bytes sit after the fields we read.

## First hardware test (suggested)

There are two stage 1 options. Test **A** first, then **B**.

- **A (lowest risk):** the blob's proven `spl.bin` with our RO stage 2.
- **B (no binary blobs):** `tools/burner-build/out/spl-x2000e/spl-x2000e.bin`. This is k1-debian's `burner_x2000` (x2000_v12) SPL plus a header generated by `tools/splhdr`, with DDR at 500 MHz, the same clock the K1's normal U-Boot uses. If A works and B doesn't, the difference is the SPL or the DDR parameters. The serial log (UART2) prints the DDR init.

For B, replace `spl.bin` with `spl-x2000e.bin` in step 3.

1. Connect the serial console: UART2, 115200 8N1, on the header by the reset/boot buttons (K1).
2. Hold BOOT, tap RESET, release BOOT. `lsusb` should show `a108:eaef`.
3. Build ballaswag's `usbboot` from source, then run:
   ```sh
   ./usbboot --stage1 spl.bin --wait 1 --stage2 tools/burner-build/out/nocry_ums_ro_x2000/u-boot.bin
   ```
4. Expected results:
   - Serial shows `Board: nocry-ums-ro`, `ums: mmc0 capacity … (read-only)`, `GADGET DRIVER: usb_dnl_ums` and `dwc pull on`.
   - The host sees `a108:eaef` disconnect, then `1209:0001` appear as a mass-storage device of about 7.3 GiB (`lsblk`, `dmesg`).
5. Back up with `dd if=/dev/sdX of=k1-full.img bs=4M status=progress conv=fsync`, then `sha256sum`. Inspect the partition table with `sgdisk -p k1-full.img`.

### If it doesn't work

| Symptom | Likely cause |
|---|---|
| No serial output | Stage 2 didn't start, or the UART params are wrong. Re-check with the blob burner first. |
| Serial OK but no re-enumeration | The DCTL disconnect/connect timing. Try a longer `mdelay`, or check `dwc pull on` in the log. |
| Enumerates but I/O errors or stalls | The 2013 f_mass_storage is running on this UDC. Look at endpoint/FIFO setup (`dwc_fifo_allocate`) and transfer sizes. |
| Capacity 0 | `mmc_init` failed. Check MSC0 pin setup (`CONFIG_JZ_MMC_MSC0_PF`). |

Every failure mode is RAM-only. Power-cycle and the board boots normally.
