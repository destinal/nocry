# The `rtos` partition and the board's boot chain

What runs between power-on and Linux on a Nebula Pad, worked out from a backup of a retail
board (firmware 1.1.0.30), Ingenic's U-Boot 2013.07 source
([k1-debian/u-boot](https://github.com/k1-debian/u-boot), `common/spl/`) and Ghidra
decompilation of the board's own SPL and rtos image. No board identity is reproduced here.

An earlier version of this document described the first 4 MiB of the disk as if it were the
rtos partition (a wrong `dd` offset). That was wrong: the "container" it described is the
boot area, and the "identity copy" was simply the `sn_mac` partition.

## Summary

- There is no U-Boot prompt stage in a normal start. The **SPL** (first-stage loader, in the
  boot area at 0x4400) loads the **rtos image** onto the second CPU core, waits for it to
  finish, then loads the Linux kernel itself and starts it.
- The rtos image is a small **FreeRTOS** program whose only job is the boot picture: it
  sets up the display, reads the model from `sn_mac`, shows the matching Creality logo,
  turns the backlight on and reports the panel type back to the SPL.
- A slot whose rtos partition has no valid header makes the SPL **hang** before Linux.
  nocry therefore treats a slot as startable only if its rtos header parses
  (`host/core/src/rtos.rs`).

## Boot chain

| Step | Who | What |
|---|---|---|
| 1 | bootrom | loads the SPL from eMMC offset 0x4400 (`MSPL` header, DDR parameters at +0x100 with the tag `INGE`, code at +0x800) |
| 2 | SPL | runs from 0x82401000 (data through the 0xb2401000 alias); sets up clocks, DDR, eMMC |
| 3 | SPL | reads the first sector of `ota`; `ota:kernel2` selects `rtos2`/`kernel2`/`rootfs2`, anything else the first set |
| 4 | SPL | reads the rtos header, checks it, loads the image to `img_start`, loads the whole `sn_mac` partition to the top of the rtos heap |
| 5 | SPL | releases CPU core 1 into the rtos (`CCU_RER` = trampoline, `CCU_CSRR` = 0) and waits |
| 6 | rtos, core 1 | display up, logo decoded and shown, backlight on; writes the panel id to the shared word; sets its own first word to 1 |
| 7 | SPL | sees the first word change, takes the panel id, waits for core 1 to sleep and resets it |
| 8 | SPL | loads the uImage from `kernel` or `kernel2` and starts Linux with the command line below |

The SPL finds partitions in a **table compiled into it**, not in the GPT on the disk. On
this board it is identical to the GPT (xboot 0, ota 0x100000, sn_mac 0x200000, rtos
0x300000, rtos2 0x700000, kernel 0xb00000, kernel2 0x1300000, rootfs 0x1b00000, rootfs2
0x20f00000, rootfs_data 0x40300000, userdata 0x52f00000). Changing the GPT does not move
what the SPL loads.

Kernel command line as built by the SPL (the bracketed placeholders in the binary are filled
in at run time):

```
console=ttyS4,115200n8 mem=<min(RAM,256)>M@0x0 [mem=<rest>M@0x30000000] lcm_id=<n> init=/linuxrc root=/dev/mmcblk0p7 rootwait rootfstype=squashfs ro
```

`root=/dev/mmcblk0p8` for slot B. `lcm_id` is the value the rtos reported. No memory stays
reserved for the rtos; it is finished before Linux starts.

The boot area also holds a U-Boot proper (2013.07, "halley5" board, at disk offset 0xa400).
The normal path above never enters it.

## rtos image header

`struct rtos_header` in `common/spl/spl_rtos.h`; all words little-endian.

| Offset | Field | Retail 1.1.0.30 | Meaning |
|---|---|---|---|
| 0x00 | `code[2]` | `0x10000008`, 0 | MIPS branch over the header, and its delay slot |
| 0x08 | `tag` | `0x534f5452` (`"RTOS"`) | checked by the SPL; `0x52544f53` on secure-boot boards (signed image) |
| 0x0c | `version` | 1 | bit 0: SPL waits for the image to change its first word, then parks core 1 |
| 0x10 | `img_start` | `0x86000000` | load and entry address |
| 0x14 | `img_end` | `0x86067948` | `img_end - img_start` bytes are loaded, rounded up to sectors |
| 0x18 | `heap_start` | `0x86070400` | |
| 0x1c | `heap_end` | `0x86fa1200` | `sn_mac` is loaded to `heap_end - partition size` |
| 0x20 | `mapped_rtosdata_size` | `0x86070400` | address of a word: SPL writes the `sn_mac` size there, the rtos writes the panel id back |

SPL checks: `tag` must match, `img_start < img_end`. Either failure prints
`rtos bad tag` / `rtos bad off` on the serial console and hangs. A failed `sn_mac` read is
only reported.

The image is the raw program, header included, so "rebuilding" an rtos partition is writing
the firmware's `zero.bin` at the start of the partition; there is no checksum or container.

## What the rtos image is made of

MIPS32 little-endian, `mips-linux-gnu-gcc 7.2.0` (Ingenic Release 5.1.0), newlib. Source
paths left in the binary:

- **FreeRTOS** kernel (`os/freertos/tasks.c`, `queue.c`, ...) with Ingenic's thread, mutex
  and wait-queue wrappers.
- **RT-Thread DFS** file-system layer (`filesystem/dfs*.c`), used for a RAM disk holding
  the `sn_mac` data.
- Ingenic's XBurst2 / X2000 drivers: clocks, GPIO, DMA, PWM, UART, display controller
  (`lcdc.c`, DPU), MIPI-DSI, a 480x272 panel driver, PWM backlight.
- **IJG libjpeg** for the logos.

## Where the source comes from

The image is built from **Ingenic's FreeRTOS SDK for the X2000** ("深度FreeRTOS平台", the
"deep FreeRTOS platform"), with Creality's boot-logo application on top. Ingenic documents it
at <http://docs.ingenic.com.cn/> (Darwin / X2000, "RTOS开发平台"). The SDK is fetched with
`repo` from Ingenic's own Gerrit server (`mirror/freertos/manifest.git`) and access requires
registering an SSH key with Ingenic, so it is **not publicly downloadable**.

Ingenic's public bare-metal library, [libbare-cpu](https://gitee.com/ingenic-dev/libbare-cpu),
supports the same chip and has FreeRTOS ports, but it is a different code base: none of the
file or function names in the board's image (`os/freertos/thread_waiter.c`,
`arch_request_irq_disabled`, `xburst2/soc-x2000/...`) occur in it.

Licences of the parts, as far as they can be told from the binary:

| Part | Licence | Source owed with the binary? |
|---|---|---|
| FreeRTOS kernel | MIT (V10 and later; older versions a modified GPL with a linking exception) | no |
| RT-Thread DFS | Apache-2.0 (3.1 and later; GPL-2.0 with exception before) | no |
| IJG libjpeg | IJG licence | no (acknowledgement) |
| newlib | BSD-style | no |
| Ingenic drivers, Creality application | proprietary | no |
| the SPL that loads it (U-Boot 2013.07) | GPL-2.0 | **yes**; matching code is in k1-debian/u-boot |

The FreeRTOS version could not be determined; the image has no version string.

## Logo choice depends on the model in `sn_mac`

The rtos splits the `sn_mac` record at `;` (serial; MAC; model) and compares the model
string with a list built into the image. Retail 1.1.0.30:

| Model field | Picture |
|---|---|
| `CR-10 H1` | Creality, 480x800 |
| `CR-K1`, `CR-K1 Max` | Creality, 480x800, rotated |
| `Nebula Pad` | Creality, 480x272 |
| anything else, or unreadable `sn_mac` | a 658x411 stock photo of a mountain at night (Ingenic's sample picture) |

The Ender-3 V3 KE 1.1.0.12 image has the same list plus `F003` and `F005`. Consequence for
nocry: the model field and the installed firmware's rtos image must agree, or the board
shows the mountain picture during start. It still starts; only the boot picture is wrong.
Each image carries 8 JPEGs; not all are used by a given build.

## Method

`dd` the partition from a full backup; `strings` for source paths and the toolchain banner;
Ghidra 12.2 headless (SPL: first 0x6000 bytes from disk offset 0x4400, base 0x82401000 with
a byte-mapped alias at 0xb2401000; rtos: base 0x86000000); JPEG markers for the pictures.
The extracted files hold the board's serial number and MAC address and are kept outside the
repository.
