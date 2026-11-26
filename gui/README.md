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
