# The recovery Linux and nocry-rescue

This folder builds the two small Linux systems nocry runs on a board. Both run entirely from memory: a kernel with its files built in, nothing mounted from the eMMC unless an action needs it.

| Image | Started | What it is for |
|---|---|---|
| `nocry-recovery.uImage` | by nocry on a computer, over USB boot mode | the working end of the desktop tool: a fastboot-style service, adb and a serial console over USB. No screen, no network. |
| `nocry-rescue.uImage` | from the board's own storage, by holding a finger on the screen at power-on (needs the [nocryboot](../nocryboot-build) loader); or over USB like the other | the same, plus a touch interface, Wi-Fi, web pages and USB flash drives: recovery without a computer |

The design notes and the bring-up log are in [`docs/nocry-rescue.md`](../../docs/nocry-rescue.md). This page says what nocry-rescue is and how its parts fit, for people who want to use it or build on it.

## What nocry-rescue does

On the touch screen (480x272, wide by default; a button turns it a quarter turn clockwise at a time):

- **Wi-Fi**: join a network (DHCP or a fixed address), or run the board's own access point with a QR code a phone joins from; turn Wi-Fi off.
- **Slots**: see what firmware is in slot A and B, choose which starts, copy one over the other.
- **Install**: firmware from the internet (Creality's published versions, NebulaOS), downloaded and checked before anything is written.
- **USB drive**: back up and restore the whole eMMC or single partitions to a flash drive (FAT32, exFAT, NTFS), install a firmware file from the drive, format the drive, eject.
- **Advanced**: repair (file system checks, storage health, wipe the settings overlay, factory reset) and remote access (nocry over the network, adb over the network, SSH: all off unless turned on).
- **System info**: model, slots, loader, rescue copies; touch calibration.

From a phone or computer on the same network: web pages with most of the same actions, behind a six-digit pairing code shown on the screen.

## How it is put together

```
nocryboot (loader)            finger on the screen at power-on
   └─ nocry-rescue.uImage     Linux 6.6 + initramfs, read from the last 32 MiB of rootfs / rootfs2
        ├─ S30nocry-usb       USB gadget: adb, fastboot service (nocry-fbd), serial console
        ├─ S45nocry-rescue    starts everything below
        ├─ nocry-ui           the touch interface (LVGL on the frame buffer, NS2009 touch chip)
        ├─ nocry-webd         the web pages
        ├─ nocry-wifi         Wi-Fi: saved network, access point, join, off
        ├─ nocry-usb          USB role (device for a computer, host for a flash drive), mounting
        ├─ nocry-remote       what the owner turned on: nocry-netd, adb relay, dropbear
        └─ nocry-rescue-do    every action on the board's storage, in one place
```

The rule that keeps it small: **the interfaces do no work themselves.** The touch interface and the web pages only start commands (`nocry-rescue-do`, `nocry-wifi`, `nocry-usb`, `nocry-remote`) and show what they print. So every check exists once, and anything the screen can do can be done from a shell with the same command.

| Part | Source | Notes |
|---|---|---|
| `nocry-rescue-do` | `overlay/usr/sbin/` | shell. Status, slots, copy, resets, file system checks, firmware fetch/check/install, flash drive backup/restore, rescue copies. Long work runs as a job with progress in `/run/nocry-job` and can be cancelled at safe points. |
| `nocry-fbd` | `fbd/` | the storage service: fastboot commands plus streaming read, write, hash, copy and zero with read-back. Everything that writes the eMMC goes through it or through `nocry-image`. |
| `nocry` | `host/` (the desktop tool, built for the board) | the same install engine and checks as on a computer |
| `nocry-ui` | `ui/` | one C file on LVGL 8.3 (MIT). `nocry-ui --sim SCRIPT` runs it on a PC without screen or touch chip: taps from a script, pictures to files. |
| `nocry-webd`, `nocry-netd` | `webd/`, `netd/` | web pages; the network service nocry on a computer pairs with |
| `nocry-image` | `tools/` | a range of the storage to an image file and back, with SHA-256, split files for FAT32, write-only-what-differs and read-back |
| `nocry-rescue-copy` | `tools/` | stores and checks the copies of the rescue image the loader starts, and the kept settings beside them |
| `nocry-usb`, `nocry-wifi`, `nocry-remote` | `overlay-rescue/usr/sbin/` | shell |
| kernel | `patches/`, `kernel-config.sh` | [k1-debian/linux](https://github.com/k1-debian) 6.6 with a device tree for the Nebula boards: panel, backlight, touch bus, Wi-Fi, USB in both roles |

### Where it lives on the board

Two copies of `nocry-rescue.uImage`, one in the last 32 MiB of each root file system partition. Those partitions are 500 MiB and a system image takes 105 to 158 MiB, and the firmware's updater writes only the image, so firmware updates, factory resets and a reformatted data partition leave the copies alone. The last 4 KiB of that room hold the kept settings: touch calibration, which way up, the access point's name and password, what remote access is on.

```sh
nocry rescue --install      # store both copies (board on USB), each read back and CRC-checked
nocry rescue                # are they there?
nocry rescue --remove
```

### Safety rules it keeps

- Nothing is written without a confirmation that says what is erased; long jobs say what stopping them leaves behind.
- Every write to the eMMC is read back.
- Firmware is downloaded and checked (checksum, board family, what it would change) before the confirm step.
- The serial number and network address are never shown on the screen or the web pages.
- No eFuse code is built into the kernel.
- Remote access is off unless turned on at the screen. adb over the network has no password, and the screen says so.

## Building

```sh
RESCUE=1 tools/recovery-build/build.sh [WORKDIR] [OUTDIR]     # needs docker and git, about 10 GB in WORKDIR
```

Without `RESCUE=1` only the plain recovery image is built. The first run takes about an hour (Buildroot, kernel); later runs redo what changed. Results land in `out/`.

Try an image without writing anything (board in USB boot mode):

```sh
nocry recovery --image tools/recovery-build/out/nocry-rescue.uImage
```

## Using it on another board, or in another project

- **Another Ingenic X2000 board**: the device tree (`patches/`) is where the panel, touch chip, Wi-Fi chip and USB pins are named. `nocry-ui` reads an NS2009 over `i2c-dev` itself and draws into `/dev/fb0`; another touch chip needs its own reading routine there, another panel size only needs to be at least 272 pixels on its short side.
- **The pieces on their own**: `nocry-image`, `nocry-rescue-copy`, `nocry-relay` and `nocry-touch` are single C files with no dependencies beyond libc (and libcrypto for the first). `nocry-ui`'s simulator mode and the job protocol of `nocry-rescue-do` (key=value lines: `state`, `what`, `done`, `total`, `message`, `risk`) are easy to reuse for a different set of actions.
- **Starting it**: any loader that can load a uImage and pass `nocry.rescue=touch` will do; [nocryboot](../nocryboot-build) is the one that finds it at the end of the root file system partitions.

## State

Run on a Nebula Pad: the touch interface in both layouts, Wi-Fi (join and access point), slots, install from the internet, repair, storing the copies, USB to a computer with the dual-role kernel, a flash drive mounting and a backup to it.

Written and checked only in the PC simulator or against a fake board so far: restore from a flash drive, install from a file on it, format, the remote access options (nocry over the network as a switch, adb over the network, SSH), Wi-Fi off, fixed addresses, the access point's own name and password, the QR code.

Not supported: the K1 family's panel and touch chip.

Licence: GPL-2.0-or-later. LVGL is MIT.
