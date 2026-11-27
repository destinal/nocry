# NebulaOS in nocry

[NebulaOS](https://github.com/NebulaOS-KE/NebulaOS) is an independent open-source firmware for the Ender-3 V3 KE and the retail Nebula Pad (Linux 6.6, Klipper, its own touchscreen interface). nocry lists it next to Creality's firmware and installs it the same way. Findings below are from the 1.0.5 release (2026-11-26).

## What its releases contain

Each release has three `.img` files. They hold the same three parts, bit for bit (checked by unpacking all three of 1.0.5); only the wrapper's file name differs, because that name is what an updater looks at:

| File | Meant for |
|---|---|
| `NebulaOS-Ender3V3KE-<version>.img` | a KE that already runs NebulaOS (its own updater) |
| `NebulaOS-Ender3V3KE-F005_ota_img_V9.<version>.img` | Creality's updater on a KE |
| `NebulaOS-NebulaPad-NEBULA_ota_img_V9.<version>.img` | Creality's updater on a retail pad |

nocry does not go through an updater, so it lists one entry, the plain file. The `.ingenic` package and the Ender-3 V3 SE mainboard files are not listed.

## How nocry recognises it

- The system is a zstd-compressed squashfs (Creality's are gzip or lzo); `squashfs.rs` reads it with a pure-Rust decoder.
- There is no `etc/ota_info`. nocry reads `usr/lib/os-release` (`etc/os-release` is a link to it): `ID=nebulaos` and `VERSION_ID`. Internally it gets the firmware id `NEBULAOS` (`boards::NEBULAOS`).
- Its start program (the `rtos` part, 0.4 MiB) and kernel carry the headers nocryboot's whole-slot check looks for, so a slot with NebulaOS counts as complete.

## What nocry does differently for it

- **Printer type is left alone.** NebulaOS ships one image for both printers and does not read the `sn_mac` record, so the model field is not changed. nocry offers it on boards set up as an Ender-3 V3 KE or a retail Nebula Pad and refuses on others (a CR-10 SE or Ender-5 Max would get the wrong printer configuration and mainboard firmware).
- **No nocry boot picture.** nocry's boot picture program replaces the firmware's start program and is proven with Creality's firmware only. A NebulaOS slot keeps its own, at install time and under Advanced > Boot picture.
- **No start trial.** nocry's success report is a script in the settings overlay (`rootfs_data`), which NebulaOS does not mount: its root is the read-only squashfs and its data lives under `/usr/data/nebulaos` in `userdata`. A trial would always fall back, so `slots --try` refuses a NebulaOS slot. NebulaOS has its own one-time fallback after an install from its own updater.
- **Clean install** blanks `userdata`. NebulaOS creates the filesystem itself when the partition is provably blank (from 1.0.4; earlier versions did not, and hung). nocry therefore lists releases from 1.0.4 on: the rule in code is "published for the retail pad too", which starts with the same version.

## Things a user should know

- NebulaOS uses its own firmware on the printer's mainboard. Its start-up scripts say that starting Creality's firmware afterwards flashes Creality's mainboard firmware back. Switching between a NebulaOS slot and a Creality slot is therefore not free of side effects; nocry's install dialog points at the release notes.
- Both slots share `userdata`. NebulaOS keeps to `/usr/data/nebulaos` and leaves Creality's files alone.

## Downloads

The list comes from GitHub's releases API (`catalog::nebulaos_from_releases`); each file is verified against the sha256 GitHub publishes for it, and a NebulaOS download without a known checksum is refused. If GitHub cannot be reached during "Check for new versions", the versions already known are kept.

## Not tested yet

Everything above was tested on a PC against a disk image: download, recognition, install to slot A and to the other slot, the refusals. NebulaOS has not been started from a nocry install on a board, and it has not been started through the nocryboot loader.
