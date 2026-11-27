# Recovery paths in the stock firmware, and an open bootloader

What the stock SPL, U-Boot and rtos can do for recovery, worked out from the decompiled boot
area of a Nebula Pad and a K1 mainboard (Ghidra, base images from `nocry backup`) and Ingenic's
GPL U-Boot source. No board identity appears here.

## Boot area layout (both families)

The first partition (`xboot`, 1 MiB) holds, in order: an `INGE` header, the **first-stage SPL**,
and a **full U-Boot 2013.07** ("halley5" board). The SPL is what the bootrom runs; it does DDR
bring-up, then (see `docs/rtos-partition.md`) loads the rtos image and the Linux kernel itself.

## Recovery paths

### 1. Bootrom USB boot — the only always-available one
The SoC's mask-ROM enters USB boot (`a108:eaef`) when it cannot boot from eMMC, or immediately if
the eMMC is blank. This is what nocry drives. Nothing on the eMMC is needed, so it is the true
bottom: a fully erased board still enters it.

### 2. The full U-Boot is present but **dormant**
The U-Boot in the boot area is a complete, interactive bootloader. Its commands (read from the
image) include:

- **Serial download**: `loadb` (kermit), `loady` (ymodem), `loads` (S-Record) — load code over the UART.
- **`softburn`** — "Ingenic usb soft burn", a USB download/burn from within U-Boot.
- **Network boot**: `tftp`, `dhcp`, `bootp` — fetch and boot an image over Ethernet.
- **Storage**: `mmc dev/part/list/erase`, `fatload`/`ext2load`/`ext4load`/`ls`.
- **Environment**: `env print/set/save/import/export/default`, `run`, `boot`, `bootd`.
- **Memory**: `md`, `mw`, `cp`, `cmp`, `crc32`, `go`, `bootm`, `boota` (Android).
- An **"Hit any key to stop autoboot"** prompt on the UART.

**But the stock SPL never jumps to it.** The SPL's boot decision (decompiled) is only: read the
`ota` record, pick `rtos`/`rtos2`, check the `RTOS` tag, run the rtos, then load the kernel. It
would chain to U-Boot only on a gzip-compressed kernel header, which these boards do not have.
So this recovery shell exists on every board and cannot be reached in the normal flow without
either replacing the SPL or loading a U-Boot yourself over bootrom USB boot (which is what nocry's
own stage2 images do).

### 3. No key-combo, no watchdog rollback
- The first-stage SPL has **no** button/GPIO/magic recovery trigger. There is no "hold a key at
  power-on" path; the only entry without a working eMMC is bootrom USB boot.
- Slot selection is purely the text in the `ota` partition (`ota:kernel` → A, `ota:kernel2` → B),
  which nocry reads and writes. The U-Boot SPL source has a watchdog feature that boots the other
  slot after a failed OTA (`spl_test_ota_result` / `spl_ota_set_flag_and_boot_wdt`), but it is
  **compiled out** on both the Nebula and the K1: there is no automatic A/B rollback. A board that
  fails to start slot A does not fall back to slot B on its own.

### 4. The rtos has no hidden shell
The rtos "console thread" is log output plus crash/assert helpers (`dump_mem32`, `dump_stack`,
`dump_buffer`), not an interactive command interface. ("At marker 0x%02x, recovery action %d" is
a libjpeg message, not a board function.) Its only job is the boot picture and the hand-over.

## What the rtos is built from (deviations from stock FreeRTOS)

The rtos is a stock **FreeRTOS** kernel (the `configASSERT` text in `tasks.c`, `queue.c` and the
semaphore code is verbatim FreeRTOS; `xQueueOverwrite` is present, so V8.0 or later; no version
string is embedded) with Ingenic additions that are **not** part of FreeRTOS:

| Layer | Files | What it is |
|---|---|---|
| POSIX-ish threading | `os/freertos/thread.c`, `thread_cond_and_mutex.c`, `critical_thread_cond.c`, `wait_queue.c`, `thread_waiter.c`, `os.c` | a Linux-like `thread`/`mutex`/`cond`/`wait_queue` API on top of FreeRTOS tasks |
| Virtual filesystem | `filesystem/dfs*.c` | **RT-Thread's DFS**, plus a `conn_fs` POSIX file-record layer |
| C library | `newlib/lock.c` | newlib, not FreeRTOS |
| Graphics | IJG libjpeg | the boot logos |

This is Ingenic's "deep FreeRTOS platform" SDK (private; see `docs/rtos-partition.md`). The Nebula
and K1 rtos images are the same code base; the only source-file difference is the LCD panel
drivers (one 480x272 panel vs. the K1's four 480x800 panels).

## An open bootloader

The pieces for an open replacement already exist in this repo. `tools/burner-build` builds the SPL
and U-Boot stages from GPL source (k1-debian/u-boot) — that is how nocry's own fastboot, USB-disk
and recovery stage2 images are made, loaded over bootrom USB boot. `rtos/` is an open replacement
for the rtos image (boot picture + hand-over), working on the Nebula and built for the K1.

A full on-eMMC open boot chain would add:
- an **SPL** of our own that does DDR bring-up and the rtos/kernel hand-over (the stock one's job),
  built from the same GPL source;
- deliberately exposing a **recovery entry** the stock SPL hides: a key-combo or a short UART
  window that drops into U-Boot (serial/USB/network download) or straight into nocry's recovery
  Linux — so a board can be rescued without a host at all;
- keeping the eFuse/secure-boot allow-list nocry already enforces.

The dormant U-Boot above shows the hardware supports all of this; the stock firmware simply does
not wire a door to it. An open SPL can.

The whole open boot loader is called **nocryboot**. Its first stage (SPL) now exists: see
[nocryboot.md](nocryboot.md).
