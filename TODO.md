# To do

Open work on nocry, newest state first. Checked items are removed after a while; the history is in git.
Last updated: 2026-11-27.

## In progress

- [ ] **Release v0.1.0-alpha.4.** Version bumped; trial run of the release workflow building. Tag when it passes.

## Needs a board in USB boot mode (minutes each)

- [ ] **K1 test board: update the loader with the installer.** It carries a hand-installed pre-0.1 nocryboot and no kept factory copy: `nocry loader --install --file backups/k1-first3M-before-nocryboot.bin --yes`.
- [ ] **Nebula test board: fill slot B** ("Copy slot A to slot B"). Empty since the last full erase, so nocryboot has nothing to fall back to and a trial cannot be armed.
- [ ] **Go back to Creality's boot picture once on hardware** (`nocry picture --firmware`). Only run on a disk image so far.
- [ ] **GUI paths on hardware:** the Install firmware tick box for the boot picture, and Advanced > Boot picture. Only the command line was used on a board.
- [ ] **Whole-slot fallback and count of starts on the Nebula.** Exercised on the K1 only.
- [ ] **ESC at the serial console** opens nocryboot's door: emulator only.

## Waiting for the replacement K1 screen

- [ ] **Real K1 panels with nocry-rtos:** detection of the four 480x800 panels is untested. When it works, set the K1 family to "proven" so the nocry picture becomes its default too (`picture_proven` in `host/core/src/session.rs`).
- [ ] **K1 test board slot B:** still holds a test build with a shifted picture (Nebula-screen experiment); give it the proper K1 image.
- [ ] **K1-family firmware install on hardware.** Tested on disk images only.

## Other hardware checks

- [ ] **Windows driver setup against a real board.** Never tested.
- [ ] **Photo of the Nebula Pad's BOOT/RESET buttons** for the USB boot help (only a K1 schematic exists).

## nocry-rescue (touchscreen recovery on the printer; design in `docs/nocry-rescue.md`)

- [ ] **Step 1: hardware bring-up from RAM on the Nebula Pad.** Done: screen, backlight, touch chip answers, Wi-Fi chip detected. Left: Wi-Fi firmware and tools in a rescue initramfs (join a network, try access-point mode); read real touches and calibrate.
- [ ] Step 2: interface skeleton, `nocry-fbd` over TCP with pairing, mDNS, nocry over the LAN.
- [ ] Step 3: stored as a file in `userdata` with a pointer in `ota`; nocryboot loads it; touch trigger in the boot picture program.
- [ ] Step 4: slots and factory reset on the touchscreen; start automatically when no slot can be started.
- [ ] Step 5: K1 family. Step 6: firmware install on the printer.

## Code, no board needed

- [ ] **Trials in the GUI,** and arm one automatically when firmware is installed to the other slot. CLI only today (`nocry slots --set b --try 3`).
- [ ] **"Restart into USB boot mode" from a running printer** (the SoC's software boot select; the firmware has `devmem`). Untried.
- [ ] **nocryboot: read the partition table from the disk** instead of the built-in one.
- [ ] **Reproducible loader builds:** the build time stamp makes every build differ.
- [ ] **Stage-2 enumeration flake:** our USB loader did not appear twice in about 35 starts; cause unknown, a retry works.
- [ ] **Install firmware tick box does not remember the last choice.**

## Decisions still open

- [ ] Code signing for the Windows and macOS packages (unsigned today).
- [ ] Real USB IDs in place of the test ones.

## Done recently

- [x] nocryboot SPL: built from public source, matched against Creality's in the emulator, installed on both test boards (2026-11-26).
- [x] Recovery door, whole-slot fallback, count of starts; all exercised on the K1 (2026-11-27).
- [x] Loader installer in the CLI and GUI; first used through the GUI on the Nebula (2026-11-27).
- [x] nocry boot picture as an install option (default on the Nebula family) and switchable later; set on the Nebula (2026-11-27).
- [x] Copy to clipboard in the serial number and MAC dialog (2026-11-27).
