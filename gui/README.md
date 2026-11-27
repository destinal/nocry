# nocry GUI (Qt Widgets)

Status: all simple-mode actions exist; advanced mode, cloud firmware list and macOS/Windows builds do not. One window that shows the board's state live, reads its identity, active system and partition table through `nocry-core`, and displays the library's log and progress. It can also take a full, verified backup with progress and Cancel. No action writes to the board yet.

```sh
cmake -S gui -B gui/build -DCMAKE_BUILD_TYPE=Release && cmake --build gui/build -j
gui/build/nocry-gui
```

Needs Qt 6 (Widgets, Concurrent), CMake and cargo. The build runs `cargo build -p nocry-ffi` in `host/` and links `libnocry_ffi.a`; the C interface is `host/ffi/include/nocry.h`. Device calls run on a worker thread; events are queued to the UI thread.

`nocry-gui --selftest out.png` reads the attached board, saves a screenshot of the window and exits (works with `QT_QPA_PLATFORM=offscreen`).

`nocry-gui --selftest-backup out.img shot.png [cancel-after-seconds]` runs a backup without dialogs and exits 0 on the expected outcome. Verified on the Nebula (2026-11-25): full backup in 5 min 12 s with the same sha256 as the CLI backup; cancel after 25 s leaves no partial file.

## Install firmware

"Install firmware…" takes a `.ingenic` file or a folder with an unpacked update (xImage, rootfs.squashfs, zero.bin), and a choice of "Keep my data" (repair/update) or "Clean install". The board is examined first; a confirmation shows what is on the board, how much will be written, what is erased, and warns about a model change or a newly generated serial number. Before writing, the first 3 MiB (partition table, ota, sn_mac) are saved under the app data folder (`safety/`); generated identities are logged there too.

`nocry-gui --selftest-install <firmware> <keep:0|1> shot.png` plans and prints the result; it only writes when `NOCRY_TEST_TARGET` names an image file. Tested that way (2026-11-25): repair of a damaged copy (bit-identical afterwards, safety copy holds the old state), a mismatched model refused with keep-data, clean install converting a retail pad image to F005, rerun = nothing to do, and a blank disk built from the `.ingenic` with a generated identity. On the real board only the planning step has run ("already installed and intact").

Not yet: a raw OTA file as input, the Creality Cloud list, fetching the boot area for a blank board when the chosen image lacks it, and reading the firmware version still needs the external `unsquashfs`.

## Switch system

"Switch system…" reads both slots (A: kernel/rootfs, B: kernel2/rootfs2), shows the firmware and version in each and which one starts, and lets the user pick the other. A slot without a kernel image and readable firmware cannot be selected (the library refuses it too). It writes Creality's `ota` record (`ota:kernel2` for B, `ota:kernel` for A), verified, after saving the first 3 MiB.

`nocry-gui --selftest-slots shot.png [a|b]` (switches only with `NOCRY_TEST_TARGET`). Tested on an image copy (2026-11-25): empty B reported and refused; with B filled, switch to B and back writes the right records. On the real board only the read has run: its slot B is empty, so there is nothing to switch to yet.

## Factory reset

"Factory reset…" blanks the data partitions the firmware recreates on first boot: `rootfs_data` (settings, system changes) and `userdata` (user files; optional "Keep the user storage partition"). Firmware, both slots, the slot choice and the identity stay. It zeroes every place an ext superblock can live, so the board's own `fsck` finds nothing and the firmware formats the partitions again; file contents are not overwritten sector by sector (this is a reset, not a secure erase).

`nocry-gui --selftest-reset shot.png <keep:0|1>` (only with `NOCRY_TEST_TARGET`). Tested on a copy of the backup image (2026-11-25): before, e2fsck accepts both filesystems; after, e2fsck finds none (rc 8) in both, and everything before `rootfs_data` is byte-identical; a second run writes nothing; with "keep user storage", `userdata` is byte-identical. Not yet run on a real board.

## Restore backup

"Restore backup…" takes a full image made by Back up. A quick check first: the file must be exactly the board's storage size and hold a valid partition table; the confirmation names the model and the last four characters of the serial number in the backup. A backup from a different board gets its own warning and button ("Restore and copy identity"). On confirm: the file is checked against its `.sha256` (if present), the first 3 MiB are saved, then only differing 4 MiB chunks are written and verified.

`nocry-gui --selftest-restore backup.img shot.png [allow]` (writes only with `NOCRY_TEST_TARGET`). Tested on image copies (2026-11-25): a converted and damaged copy restored bit-identical (16 MiB written), rerun writes nothing, another board's backup refused then accepted with `allow`, a damaged backup file refused by checksum with nothing written, a non-backup file refused. On the real board only the check step has run.

## Main window (2026-11-25)

A status line, then three tabs. Home: "This board" (model, firmware and version in the active slot, the other slot, serial, MAC, mainboard, storage) and the actions as described buttons. Partitions: the table. Activity log. Progress and Cancel sit below the tabs. The details fill in by themselves when a board appears, clear when it is unplugged, and refresh after any action that changed the board; a failed read is not retried in a loop (Refresh does it manually).

## Firmware list (2026-11-26)

Install firmware now offers "Download from Creality": a printer and a version are chosen from a list, with the board's own model preselected and the release notes shown. The list is built into the program (`host/core/data/catalog.json`) and "Check for new versions" refreshes it from Creality's API into the user's data folder. Downloads go to `<data>/firmware`, are verified against the md5 in the download address, and are reused. "Use a file on this computer" keeps the previous behaviour.

Command line: `nocry catalog [--refresh]`, `nocry fetch <MODEL> <VERSION|latest>`.

Tested on image copies through the GUI's self-test (`--selftest-install-list`): a retail copy updated from 1.1.0.23 to the downloaded 1.1.0.30 keeping data; a repeat neither downloads nor writes again; a clean install of the downloaded Ender-5 Max firmware converts the copy. Not yet run against a real board.

## Shell status

While the recovery runs, a line under the status shows "ADB: ready / not installed / not connected" and "Serial console: <port>", with "How to get a shell…" opening per-platform instructions and copyable commands. Detection is in `session::shell_status`: `adb devices` for ADB; for the console, sysfs on Linux, `/dev/cu.usbmodem*` on macOS, the registry (via `reg query`) on Windows. Verified on Linux with the Nebula; the macOS and Windows detection paths have not been run.

## Advanced tab (2026-11-26)

- **Back up / Restore / Erase partition** for the selected row. Backup writes the file and a `.sha256`. Restore writes a file from the start of the partition (it may be shorter, never longer), only where it differs, verified. Erase fills with zeros.
- **Edit serial number and MAC…**: 14 and 12 hex digits (":" and "-" accepted in the MAC), model, mainboard, and "Generate new random values". The changes are listed for confirmation before writing.
- **Erase entire storage…**: two confirmations, the second by typing ERASE. Everything becomes zeros; the board is blank.
- **Blank board**: Install firmware detects a board without a partition table and rebuilds it. If the chosen firmware has no boot area (update files do not), `catalog::boot_area` downloads Creality's `Ender-3_V3_KE_1.1.0.12.ingenic` from their GitHub releases once, extracts `u-boot-with-spl-mbr-gpt.bin` and checks it against a pinned sha256.

Every write first saves the first 3 MiB to `<data>/safety`.

Tested on a copy of the backup image with `--selftest-adv` (2026-11-26): partition backup equals the image and its checksum verifies; restore into `kernel2`; an oversized file refused; erase to zeros, image identical to the original afterwards; invalid MAC refused, valid identity written; full erase leaves every byte zero; the blank image rebuilt from the downloaded Nebula 1.1.0.30 update with 10 partitions, Creality's bootloader, a generated identity and nothing left to repair; original identity restored byte-identical. None of this has been run on a real board yet.

## Over the network

A board that runs nocry-rescue (see `docs/nocry-rescue.md`) can be used without a USB cable: **Connect over the network…** finds it, asks for the pairing code on its screen, and from then on every action works on that board. Self-tests: `--selftest-net-dialog shot.png`, `--selftest-net ADDRESS CODE shot.png`.

## Progress and Cancel

While anything runs, the bar under the tabs is shown: moving until the operation reports how far it is, then filling. Cancel is offered for everything that can take long:

| Operation | Cancel |
|---|---|
| Backup, partition backup, firmware download, the checks before an install or restore | stops at once; nothing on the board was changed |
| Install, install to the other slot, restore a backup, copy a slot, factory reset, secure erase, partition restore or erase, erase all | asks first, saying what stopping half-way leaves behind (for example: the board will not start until the installation is run again) |
| Boot loader, boot logo, identity, switching the slot, reading the board | none: they take seconds, and the first three must not be cut short |

A stop takes effect at the next safe point (between two writes), never in the middle of one. Self-tests: `--selftest-cancel-question shot.png`; `NOCRY_TEST_CANCEL_MS=n` presses Cancel n ms after a risky operation offers it.
