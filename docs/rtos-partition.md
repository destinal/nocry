# The `rtos` partition: an early boot + instant-logo stage

Findings from analysing a Nebula Pad's `rtos` partition (offset `0x300000`, size
4 MiB; a second copy lives in `rtos2` for slot B). Everything here was read from a
backup of a retail board; no board identity is reproduced in this document.

## Why it exists

`rtos` is not application firmware. It is the stage that runs **before** Linux: it
brings up DRAM and clocks, paints the Creality splash on the panel within
milliseconds of power-on, and then loads and starts the main Linux kernel. The RTOS
splash and the U-Boot that boots Linux are packed together in this one partition.

## Container format (`INGE`)

The partition is an Ingenic `INGE` container (magic `"INGE"`, `0x45474e49` LE at
offset 0). It holds two payloads plus a copy of the identity record:

| Region | File offset | Size | Contents |
|---|---|---|---|
| Outer `INGE` header + load map | `0x0000` | 4 KiB | entry records, each 16 bytes; load markers at `0x10000000` |
| Payload A | `0x4000` | ~192 KiB | U-Boot (a nested `INGE` at `0x4500`) |
| Identity record | `0x200000` | 4 KiB | `sn_mac` string, duplicated here so early boot can read identity even if `sn_mac` is damaged |
| Payload B | `0x300000` | ~416 KiB | the FreeRTOS splash image |

The outer header's 16-byte entry records are not fully decoded; the load address
`0x10000000` and the per-entry sizes are legible, the remaining fields are not yet
confirmed. Payload B begins with its own small header: `[entry=0x10000008][0]["RTOS"][version=1]`.

## Payload A — U-Boot 2013.07 (the "quasi-bootloader")

- Ingenic "halley5" board port for the XBurst2 **X2000-V12** SoC, built Oct 2023.
- Does DRAM bring-up itself (Innophy DDR PHY calibration; DDR2/DDR3/LPDDR/LPDDR2/LPDDR3),
  so this is the combined SPL + U-Boot, not a thin loader.
- Full U-Boot command set and a network stack (`md`, `mw`, `crc32`, `mmc`, `dhcp`,
  `env import/export`, `bootm`).
- Loads and launches payload B: strings `rtos not found: rtos`, `rtos bad tag: %x`,
  `rtos bad off: %lx %lx`, `read rtos image err`.
- Boots Linux with per-slot bootargs:
  `console=ttyS4,115200n8 … init=/linuxrc root=/dev/mmcblk0p7 rootwait rootfstype=squashfs ro`
  for slot A, and `…/dev/mmcblk0p8…` for slot B, chosen from `Current Version: V1 / V2`
  (the same A/B choice nocry writes into the `ota` partition as `ota:kernel` / `ota:kernel2`).

## Payload B — the FreeRTOS splash

MIPS32 little-endian, built with `mips-linux-gnu-gcc 7.2.0` (Ingenic Release 5.1.0,
xburst2 fp64), newlib C library. Source paths left in the image show what it is built from:

- **FreeRTOS** kernel: `os/freertos/{tasks,queue,semaphore,thread,...}.c`; the symbol
  `eTaskConfirmSleepModeStatus` is FreeRTOS tickless-idle. A small POSIX-like wrapper
  (`thread_cond_and_mutex.c`, `wait_queue.c`, `thread_waiter.c`) sits on top.
- **RT-Thread DFS** virtual filesystem (`filesystem/dfs*.c`) reused on top of FreeRTOS.
- **Ingenic XBurst2 / X2000 BSP**: `xburst2/{cache,cpu,irq}.c`,
  `xburst2/soc-x2000/{clk,dma,gpio,lcdc,mipi_dsi,pwm,uart}.c`.
- **IJG libjpeg** decoder, and a **DPU → MIPI-DSI → panel** display pipeline with a
  PWM backlight driver. Panels seen: 480×272 and 480×800, portrait and landscape.

What it does at runtime: threads named `boot logo thread`, `console thread` and
`enbale_fb_thread` (sic) bring up the framebuffer and decode one of **8 embedded JPEG
logos** (`creality_{portrait,landscape}_rot{0,180,270}_{480x272,480x800}`, SOI markers
from `0x3f064`) for the panel's resolution and rotation, then display it. No touch
input, rpmsg or inter-core mailbox strings are present, so it does not talk to Linux;
it simply runs first and hands the lit panel over for a seamless logo through to the UI.

## How this was found

`dd` the partition out of a full backup; `strings` for source paths, the toolchain
banner and symbols; a Capstone coverage sweep to confirm MIPS32 little-endian (100% of
payload B decodes as MIPS32 LE, ~3% as big-endian or microMIPS); JPEG SOI scan for the
logos. Scripts and carved payloads are kept out of the repo (under `~/nocry-work/`).
