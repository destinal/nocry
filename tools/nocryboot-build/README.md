# nocryboot

nocryboot is an open first-stage boot loader (SPL) for boards built on the Ingenic X2000E, written for Creality's Nebula Pad and K1 families. It replaces the closed loader those boards ship with. It is the 24 KiB the processor's boot ROM reads from byte 0x4400 of the eMMC; it sets up clocks and memory, shows the boot logo and starts Linux.

It does what the factory loader does, and adds three things the factory loader lacks:

| | Factory loader | nocryboot |
|---|---|---|
| A firmware slot is damaged | hangs | starts the other slot |
| Neither slot can start | hangs; BOOT button on the mainboard needed | restarts the board in USB boot mode by itself, so a computer can repair it |
| A new firmware should be tried safely | not possible | a slot can be put on trial for a number of starts; if it never comes up, the board goes back to the other slot |
| Recovery without a computer | none | a finger on the screen at power-on starts a rescue system stored on the board (Nebula Pad) |

The design notes, measurements and test log are in [`docs/nocryboot.md`](../../docs/nocryboot.md). This page is the short version.

## How a start goes

1. Clocks, console UART, memory (LPDDR2).
2. *(Nebula Pad build)* The touch chip is read. Three pressure readings 20 ms apart, all pressed, mean "rescue": go to step 7.
3. The slot record is read from the first sector of the `ota` partition: `ota:kernel2` means slot B, anything else slot A.
4. The slot is checked: kernel, boot logo image and root file system each have to carry their signature. If the slot is incomplete, or its trial count has run out, the other slot is taken and the record rewritten. If no slot is complete, the board restarts in USB boot mode.
5. The slot's boot logo image (`rtos` partition) runs on the second processor core and switches the display on.
6. The slot's kernel is loaded and started.
7. *(rescue)* A copy of the rescue system is looked for in the last 50 MiB of each root file system partition, the active slot's first. A copy is a plain uImage named `nocry-rescue...`; it is started only if the CRCs in its header match the header and the contents. If neither copy is good, the start goes on at step 3.

The loader never writes to the eMMC except the first two sectors of `ota` (the slot record and its own note about trials).

## Building

```sh
tools/nocryboot-build/build.sh        # needs docker and git
```

This fetches [k1-debian/u-boot](https://github.com/k1-debian/u-boot) (Ingenic's U-Boot 2013.07 tree) at a pinned revision, applies `patches/*.patch` and builds four files into `out/`:

| File | For |
|---|---|
| `nocryboot-nebula.bin` | Nebula Pad family (console on UART4, touch-to-rescue) |
| `nocryboot-k1.bin` | K1 family (console on UART2) |
| `nocryboot-doortest-*.bin` | test loaders that only restart the board into USB boot mode |

The build fails if the result is not exactly 24 KiB or if the code references the SoC's eFuse block: nocryboot never touches eFuses.

## Trying it without installing

```sh
nocry run-spl tools/nocryboot-build/out/nocryboot-nebula.bin     # board in USB boot mode
```

The loader is sent to the boot ROM over USB and run from memory. Nothing is written; it then starts whatever is on the eMMC.

`tools/nocryboot-analysis` runs a loader in an emulator against a copy of a board's storage and logs every register access, console line and eMMC read. That is how nocryboot was compared with the factory loader, and how its decisions (damaged slots, trials, rescue copies) are tested.

## Installing and going back

```sh
nocry loader                      # what the board has
nocry loader --install --yes      # install nocryboot
nocry loader --factory --yes      # put the factory loader back
```

The factory loader is kept on the board before it is replaced (in the unused end of the first MiB) and as a file on the computer. There is only one loader on the eMMC: if a new one does not start, the board stays dark until the BOOT button brings it into USB boot mode, from where `--factory` works.

## Using it on another board

nocryboot is specific to these boards in four places, all in the patches:

| What | Where | Notes |
|---|---|---|
| Memory type and timing | `include/configs/nocryboot.h` and the SPL header made by `tools/splhdr` | one memory type is built in (W97BV6MK LPDDR2); another chip needs its parameters |
| Partition table | `board/ingenic/x2000_base/nocryboot/<family>` | compiled in; the loader does not read the table on the disk. `ota`, `sn_mac`, `rtos`, `rtos2`, `kernel`, `kernel2`, `rootfs`, `rootfs2` |
| Console UART and pins | `nocryboot.h`, per family | |
| Touch chip for rescue | `common/spl/spl_nocryboot.c` | NS2009 on bit-banged I2C, pins PC25/PC26; another panel needs its own "is it pressed" routine, or leave `CONFIG_NOCRYBOOT_TOUCH` off |

What a system has to provide to be started by it:

- **Slots.** Two sets of `rtos`, `kernel`, `rootfs` partitions and a 512-byte text record in `ota` naming the active one.
- **Boot logo image.** A small bare-metal program with Ingenic's `RTOS` header that initialises the display. nocry's open one is in [`rtos/`](../../rtos).
- **A rescue image** (optional). Any Linux uImage that needs no unpacking by the loader (a self-extracting kernel with built-in initramfs), named `nocry-rescue...`, at most 50 MiB minus 4 KiB, written to the last 50 MiB of a root file system partition. The loader passes it `nocry.rescue=touch` on the kernel command line. [`tools/recovery-build`](../recovery-build) builds the one nocry uses.

## State

- Slot fallback, USB boot mode on its own and trials: tested in the emulator and on a K1 mainboard and a Nebula Pad.
- Touch-to-rescue: version 0.2 (rescue kept as a file on the data partition) and 0.3 (two copies behind the system images, CRC-checked, 32 MiB each; never released) ran on a Nebula Pad. Version 0.4 is 0.3 with 50 MiB of room; it passes every emulator case and has not been installed on a board yet.
- The K1 family has no touch-to-rescue: its panel and touch chip are not supported.

Licence: GPL-2.0-or-later, as the U-Boot source it is built from.
