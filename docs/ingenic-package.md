# `.ingenic` recovery packages (Ingenic Cloner) and what the OTA leaves out

Reference: `Ender-3_V3_KE_1.1.0.12.ingenic` from
https://github.com/CrealityOfficial/Ender-3_V3_KE_Klipper/releases/tag/V1.1.0.12
(sha256 `5388b16810e51c8233d6ee978b5b4a09347a4c9a4a516d3c5bf8c686e6783f3c`, 130 MB).

## Format

A plain zip containing the whole Cloner tool tree. Everything needed for X2000E:

| Path | What |
|---|---|
| `images/u-boot-with-spl-mbr-gpt.bin` (212296 B) | **Boot area**: MBR + GPT + SPL + U-Boot (`Board: halley5 (Ingenic XBurst2 X2000-V12 SoC)`, U-Boot 2013.07). This is **not in the OTA**. |
| `images/zero.bin` | RTOS image (`RTOS` magic at +8). Also in the OTA; it is real, not zeros. |
| `images/xImage`, `images/rootfs.squashfs` | Same as the OTA. |
| `firmwares/x2000/{spl.bin,uboot.bin,config.cfg}` | The Cloner's USB-boot SPL and burner (`burner_x2000`, built Dec 2023). These are newer builds than ballaswag's blobs and hash differently. |
| `configs/x2000/x2000e_mmc0_lpddr2_linux.cfg` | The Creality burn policy (below). |
| `ddrs/LPDDR2/W97BV6MK_LPDDR2.cfg` | DDR chip parameters for the Cloner's header generator. |
| `security/x2000/key.bin` (588 B) | Secure-boot key material. **Never burn it**: the policy has `security_burnkey=0` and `security_enable=0`. |

## Creality's factory burn policy (`x2000e_mmc0_lpddr2_linux.cfg`)

- **`[info]`:** cpufreq 800 MHz, ddrfreq **400 MHz**, extal 24 MHz, baud 115200, `uart_gpio=9`.
- **`[ddr]`:** `W97BV6MK_LPDDR2`, `current_type="2,LPDDR2"`. That's the cloner-generation enum; see `tools/splhdr`.
- **`[debug]`:** `transfer_size=65536`, `transfer_data_chk=1`, `read_back_chk=1`.
- **`[mmc]`:** `force_erase=2`, `erase_list="0x0,0x1fffff;0x300000,0xffffffff"`. **This erases the whole eMMC except `sn_mac` (0x200000–0x2fffff)**, so a factory reflash keeps the identity and wipes `ota`, `rootfs_data` and `userdata`.
- **Writes (enabled policies):**

  | Area | Offset | Image |
  |---|---|---|
  | boot area | 0x0 | `u-boot-with-spl-mbr-gpt.bin` |
  | `rtos` | 0x300000 | `zero.bin` |
  | `kernel` | 0xb00000 | `xImage` |
  | `rootfs` | 0x1b00000 | `rootfs.squashfs` |

  `ota`, `sn_mac`, `rtos2`, `kernel2` and `rootfs2` have policies, but they are disabled. That explains an empty `ota` on a factory-imaged board.

## Boot loader is shared across Nebula boards (verified)

The KE package's `u-boot-with-spl-mbr-gpt.bin` was compared against the first 212296 bytes of the user's retail Nebula Pad backup (firmware 1.1.0.23):

| Region | Result |
|---|---|
| GPT | Header (24 B) and per-partition unique GUIDs (`0x10–0x1f` of each entry) differ; these are generated per device. **The partition layout is identical.** |
| SPL + U-Boot | Only the build-timestamp strings differ ("Dec 29 2023 18:01:54" vs "Oct 20 2023 15:43:37"). The code is byte-identical. |
| Rest of xboot (to 1 MiB) | Zeros on the Nebula. |

The `rtos` and `kernel` images differ between KE 1.1.0.12 and the retail 1.1.0.23, as expected for different builds.

## Implications for nocry

- **A full restore over nocry-ums (read-write) can do exactly what the Cloner does**, without the burner: write boot area, `rtos`, `kernel` and `rootfs` at the offsets above, and clear `ota`, `rootfs_data` and `userdata`.
- **To keep the board's GPT GUIDs** when restoring the boot area, write only from 0x4400 (after the primary GPT), or restore the device's own backup.
- For any X2000E Nebula board, the boot area can come from any Nebula-family `.ingenic`. The rest comes from the target printer's OTA or `.ingenic`.
- The Cloner's 64 KiB `transfer_size` gives a reference point for the ums speed tuning (`FSG_BUFLEN`).
