# nocryboot

nocryboot is the open boot loader for Creality's Ingenic X2000E boards (Nebula Pad family, K1 family). It replaces the closed pieces between the SoC's boot ROM and Linux:

| Part | Factory | nocryboot | State |
|---|---|---|---|
| First stage (SPL), eMMC byte 0x4400 | Creality build of Ingenic's U-Boot SPL, no source published | built from public source, `tools/nocryboot-build` | builds; matches the factory SPL in the emulator; **not yet run on a board** |
| Boot picture image (`rtos`, `rtos2`) | Ingenic FreeRTOS image, closed | `rtos/` (nocry-rtos) | runs on the Nebula Pad and a K1 mainboard |
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

### Build and try

```
tools/nocryboot-build/build.sh             # -> tools/nocryboot-build/out/nocryboot-{nebula,k1}.bin
tools/nocryboot-analysis/compare.sh nebula <backup image of a Nebula board>
nocry run-spl tools/nocryboot-build/out/nocryboot-nebula.bin      # board in USB boot mode
```

`nocry run-spl` sends the loader to the boot ROM over USB and starts it from RAM. Nothing is written; the loader then boots whatever is installed. The factory loader can be started the same way (cut 24 KiB at byte 0x4400 from a backup), which is the control for this test.

Installing it means writing the 24 KiB file at byte 0x4400 of the eMMC (inside the `xboot` area). There is no installer for that yet, and no second copy of the SPL: a bad one leaves the board dark until it is rewritten from USB boot mode, which always works because the boot ROM does not depend on the eMMC.

### Next

- Run it on both boards over USB, then installed.
- A recovery door: a way to reach a loader without the BOOT button (the factory SPL has none).
- Read the partition table from the disk instead of the built-in one.
