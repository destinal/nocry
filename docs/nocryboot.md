# nocryboot

nocryboot is the open boot loader for Creality's Ingenic X2000E boards (Nebula Pad family, K1 family). It replaces the closed pieces between the SoC's boot ROM and Linux:

| Part | Factory | nocryboot | State |
|---|---|---|---|
| First stage (SPL), eMMC byte 0x4400 | Creality build of Ingenic's U-Boot SPL, no source published | built from public source, `tools/nocryboot-build` | builds; matches the factory SPL in the emulator; boots a Nebula Pad when started from RAM (`nocry run-spl`, 2026-11-26, with the factory loader run the same way as control); installed on a Nebula Pad and on a K1 mainboard (CR4CU220812S12); both cold-boot from it (2026-11-26) |
| Boot logo image (`rtos`, `rtos2`) | Ingenic FreeRTOS image, closed | `rtos/` (nocry-rtos) | runs on the Nebula Pad and a K1 mainboard |
| Full U-Boot in the boot area | present, never started | not needed | – |

## First stage (SPL)

What it does, same as the factory one (see `docs/rtos-partition.md` for the details of each step):

1. Sets up clocks, the console UART and the memory (W97BV6MK LPDDR2 at 500 MHz).
2. Reads the `ota` record: `ota:kernel2` selects slot B, anything else slot A.
3. Loads that slot's rtos image, puts the whole `sn_mac` partition at the top of its heap, runs it on CPU core 1 and waits until it reports done. An image without a valid header stops the boot (as on the factory SPL).
4. Loads that slot's kernel and starts it with
   `console=ttyS4,115200n8 mem=256M@0x0 mem=0M@0x30000000 lcm_id=<n> init=/linuxrc root=/dev/mmcblk0p7 rootwait rootfstype=squashfs ro`
   (`ttyS2` on the K1 family, `mmcblk0p8` for slot B; `<n>` is the panel id the rtos image reported, 0 if none).

Source: [k1-debian/u-boot](https://github.com/k1-debian/u-boot) at `271fbe7` (Ingenic's U-Boot 2013.07 tree) plus one patch, `tools/nocryboot-build/patches/0001-nocryboot-spl.patch`. The patch adds:

- board config `include/configs/nocryboot.h` and targets `nocryboot_nebula`, `nocryboot_k1`;
- the partition table the SPL uses (it does not read the table on the disk; the partitions it needs sit at the same place on both families);
- the panel id hand-over (`lcm_id=`), which the public source lacks;
- switches that keep the factory behaviour where the current Ingenic code has moved on (below).

### Differences from the public Ingenic code, on purpose

The factory SPL was compared with a build of the unmodified public code by running both in an emulator and logging every register access (`tools/nocryboot-analysis`). Where they differed, nocryboot follows the factory SPL:

| Topic | Public code today | Factory SPL and nocryboot |
|---|---|---|
| eFuse | reads the SoC id and a trim word to pick memory settings | never touches the eFuse block; one memory type built in (build fails if the code references eFuse) |
| Memory controller, termination | requests on-die termination | no request (LPDDR2 has none) |
| Memory controller, idle | precharge power-down | active power-down |
| Memory PHY reset | holds one more reset line during PHY setup | plain sequence |
| Pins | clears pull-downs on every pin it configures | leaves pull-downs alone |
| Console pins, Nebula | UART4 on PC11/PC12 | UART4 on PA2/PA3 |
| Kernel arguments | second memory bank only if it exists | `mem=0M@0x30000000` always present |

Remaining differences in the emulator traces: the order of two timer/clock-gate writes, one extra read of the reset status (printed on the console), and spaces in the kernel arguments.

Not compared in the emulator: the two eMMC routines "send a command" and "read blocks" (they are replaced there). The command sequence around them is identical; the routines themselves are verified on hardware.

### Recovery door

The factory loader hangs when the boot logo image of the chosen slot is missing or damaged, and has no way back to a PC except the BOOT button on the mainboard. nocryboot decides like this:

A slot is only started if all three of its parts are there: kernel (uImage header, Linux type, plausible load address and size), boot logo image (valid header; it also switches the display on, and Linux relies on that), root file system (squashfs signature). Only the first sector of each is looked at; nothing is checksummed, which would cost boot time.

1. The slot the `ota` record names, if complete.
2. Otherwise the other slot, if complete.
3. Otherwise the board restarts in **USB boot mode**, the mode the BOOT button selects, where nocry can repair it. The same happens when the eMMC does not answer, or when three ESC characters (and nothing else) are waiting at the serial console.

USB boot mode is requested the way Ingenic's own tools do it: the value `0x42575302` in the CPM register `SLPC` (0xb00000cc), then a watchdog reset.

Whenever the SPL changes slot it rewrites the `ota` record. Creality's updater takes "the slot I may overwrite" from that record (`/etc/ota_bin/ota_local_method.sh`), so leaving it stale would make the next update overwrite the running system.

#### Count of starts (trial)

A slot that looks complete can still fail: a kernel that panics, a boot logo image that hangs. For that there is a count of starts, as in other two-slot systems. It only runs when armed, because stock firmware never reports success by itself.

- **Arming** (`nocry slots --set b --try 3`): nocry stores a small start-up script in the firmware's settings area (`rootfs_data`, file `upper/etc/init.d/S98nocryboot`), writes `nocryboot:try=B:left=3` into the second sector of `ota`, and names the slot in the `ota` record. It refuses if the board's loader does not count, if the other slot is not complete (nothing to go back to), or if the settings area does not exist yet.
- **Each start** of the slot on trial takes one off the count.
- **Success**: the script is started before the printer's own programs, waits 45 seconds in the background, and erases the second sector of `ota` (and leaves one line in `/usr/data/nocryboot-confirm.log`). From then on nothing is counted.
- **Count at 0**: the SPL starts the other slot instead, names it in the `ota` record and leaves `nocryboot:failed=B`. If the other slot turns out not to be complete it keeps the slot and leaves `nocryboot:unconfirmed=B`.
- A slot left because parts were missing gets `nocryboot:incomplete=B`. `nocry slots` shows these notes; choosing a slot without `--try` clears them.

Limits: a factory reset erases the settings area and with it the script; a trial armed before that will fail and go back. The script is shared by both slots (one settings area). The count is a single digit. The SPL writes only the first two sectors of `ota` (its write routine refuses anything else).

State: all cases pass in the emulator (`splemu.py --blank ... --slot ... --console-input ... --note ... --emmc FILE` for a sequence of starts). On a K1 mainboard (2026-11-26/27): the SPL counted two starts down on the eMMC, and the started stock system (1.3.2.20) then erased the note and logged it, so both the SPL's write and the success script work there. With slot A on trial for 2 starts and power cut early twice, the third start came up in slot B with the `ota` record rewritten to `ota:kernel2` and the note `nocryboot:failed=A`. `nocryboot-doortest-<family>.bin` is a loader that only opens the door; started from RAM on a K1 mainboard (2026-11-26) the board was back in USB boot mode within half a second, without the BOOT button, and the boot ROM answered. On a K1 mainboard with the first sector of `rtos2` zeroed and slot B named, this loader (started from RAM) started slot A, with video (2026-11-26); an earlier build that started slot B without its boot logo image gave a running system with a dark screen. The current build (door, whole-slot rule, count of starts) is installed on the K1 mainboard and on the Nebula Pad; both cold-boot from it (2026-11-27). The door, the fallback and the count have only been exercised on the K1.

### Hold the screen for the rescue system (0.2, Nebula Pad)

At every start the Nebula Pad build reads the touch chip once, before it looks at the slots: an NS2009 on the two pins PC25 (clock) and PC26 (data), the I2C bus bit-banged as open-drain lines. Measured on a board: 0 when nobody touches the screen, about 2200 of 4095 under a finger. Three readings 20 ms apart of 600 or more count as held; one reading below that, or a chip that does not answer, and the board starts as always. An untouched start costs about a millisecond. The K1 build has no such check (other panel, other pins).

With the screen held, the SPL starts the rescue system (`docs/nocry-rescue.md`) instead of a slot:

- **Where it is.** A file on the data partition, `nocry-rescue/rescue.uImage`. The SPL cannot read a file system, so whoever stores the file leaves a note in the third sector of `ota`: `nocryboot:rescue:<sum> <sector>+<count> <sector>+<count> ...`, all hexadecimal: the runs of sectors that hold the file, in order, and the sum of its 32-bit words. Several runs are normal: this file system puts a bookkeeping block between every 4 MiB of a file.
- **What it must be.** A uImage named `nocry-rescue...`, a Linux kernel the loader does not have to unpack: the rescue kernel is built to unpack itself, as the firmware's kernels are.
- **Checks.** The note has to be there, the first sector has to carry that header, every run has to read, and the words have to add up to the noted sum. If the file was moved, deleted or overwritten the sum is wrong, and the board starts as always. Nothing is written.
- **Hand-over.** The kernel gets `nocry.rescue=touch`. The rescue system then shows a five-second count and wants the screen held throughout; let go and it restarts into the firmware. A stray touch at power-on costs a restart.

There is no second stage. One was tried when a USB flash disk was to be a way in as well (it needs a USB host stack, which the SPL has no room for) and dropped again with that idea; the SPL already knows how to load and start a kernel.

The loader is 244 bytes under its 24 KiB after this.

Storing the rescue system: `nocry rescue --install` (it sends `nocry-rescue.uImage` to the board, which writes the file, works out the runs, reads them back raw to check the sum and only then writes the note); `nocry rescue` says whether it is there and intact; `--remove` takes it away. A factory reset from Creality's menu or anything else that rewrites the data partition leaves a note that no longer matches, which is harmless: store it again.

Tested in the emulator (`splemu.py --touch N`): untouched, a light touch, held with the image in three scattered runs (started, with the right argument), held with one byte of the image changed, held without a note. Not yet run on a board.

### Build and try

```
tools/nocryboot-build/build.sh             # -> tools/nocryboot-build/out/nocryboot-{nebula,k1}.bin
tools/nocryboot-analysis/compare.sh nebula <backup image of a Nebula board>
nocry run-spl tools/nocryboot-build/out/nocryboot-nebula.bin      # board in USB boot mode
```

`nocry run-spl` sends the loader to the boot ROM over USB and starts it from RAM. Nothing is written; the loader then boots whatever is installed. The factory loader can be started the same way (cut 24 KiB at byte 0x4400 from a backup), which is the control for this test.

### Installing and going back

```
nocry loader                      # what the board has, and whether nocryboot can go on
nocry loader --install --yes      # install nocryboot (without --yes: say what would happen)
nocry loader --factory --yes      # put Creality's loader back
```

In the window: Advanced, "Boot loader…".

The loader is the 24 KiB at byte 0x4400 of the eMMC, inside the unpartitioned first MiB. Before nocry writes there it checks:

- the loader on the board is one it knows (Creality's or nocryboot); anything else is left alone;
- the board family (from the identity record and from the present loader's console setting; they must agree);
- the partitions the loader reads are where its built-in table says (`ota`, `sn_mac`, `rtos`, `rtos2`, `kernel`, `kernel2`, `rootfs`, `rootfs2`);
- the image to install is nocryboot for that family, and not a test build.

A factory loader is kept before it is replaced: on the board itself, at 0xf0000 in the unused end of the first MiB (with a line of text and a checksum in the sector before, 0xefe00), and as a file in nocry's data folder (`safety/`). `--factory` takes it from the board; `--factory --file X` takes it from a file, which can be a backup image of the board. For a board that got nocryboot before nocry kept copies, `--install --file <backup from before>` stores the factory loader from that backup on the board.

Every write is read back. There is one copy of the loader on the eMMC: if a new one does not start, the board stays dark until the BOOT button brings it into USB boot mode, from where `nocry loader --factory` works.

First use on a board (2026-11-27): the window's Boot loader dialog installed nocryboot 0.1 on a Nebula Pad that had just been erased and reinstalled (so it carried the factory loader of the rebuild package, built Dec 29 2023). Read back afterwards: loader identical to the shipped image, kept factory copy identical to the one saved on the PC, and the board cold-boots.

The loader carries a text nocry reads, `nocryboot-loader <version> <family>`; it is also the first line it prints on the console.

## Boot logo

The logo of the first seconds after power-on comes from the boot logo image in the slot's `rtos` partition, not from the loader. Every Creality firmware brings its own (the Creality logo); nocry's open one is `rtos/` (see `docs/rtos-partition.md`). Either works with either loader.

- **Install firmware** in the window has a tick box, on by default: "Show the nocry boot logo when the printer powers on". Unticked, the firmware's own stays.
- **Advanced, Boot logo…**, or `nocry logo [--nocry|--firmware] [--yes]`, switches every installed slot later.
- The default only takes effect on the Nebula Pad family. On the K1 family nocry's image has run on the mainboard but never on one of its own screens, and a wrong panel setup would leave the screen dark in Linux too, so there the firmware's picture stays unless chosen explicitly (`nocry logo --nocry`, or the button, which warns).

The firmware's own image is kept when it is replaced, in the unused second half of the same 4 MiB partition: the image at 2 MiB, and in the sector before it a line `nocry:firmware-picture size=<n> sha256=<hex>`. Going back copies it to the start again and needs no firmware file. A firmware update done by the printer itself rewrites the start of the partition of the slot it updates, which brings Creality's picture back for that slot.

State: tested on disk images of both boards (set, loader run in the emulator, back; install with and without the tick). First use on a board (2026-11-27): `nocry logo --nocry` on a Nebula Pad with nocryboot 0.1; it shows the nocry picture and then starts normally. Going back to the firmware's picture has not been done on a board yet.

### Next

- Read the partition table from the disk instead of the built-in one.
