# To do

Open work on nocry, newest state first. Checked items are removed after a while; the history is in git.
Last updated: 2026-11-27.

## Needs a board in USB boot mode (minutes each)

- [ ] **K1 test board: update the loader with the installer.** It carries a hand-installed pre-0.1 nocryboot and no kept factory copy: `nocry loader --install --file backups/k1-first3M-before-nocryboot.bin --yes`.
- [ ] **Nebula test board: fill slot B** ("Copy slot A to slot B"). Empty since the last full erase, so nocryboot has nothing to fall back to and a trial cannot be armed.
- [ ] **Go back to Creality's boot logo once on hardware** (`nocry logo --firmware`). Only run on a disk image so far.
- [ ] **GUI paths on hardware:** the Install firmware tick box for the boot logo, and Advanced > Boot logo. Only the command line was used on a board.
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

- [ ] **Step 1: hardware bring-up from RAM on the Nebula Pad.** Done: screen, backlight, touch chip answers, Wi-Fi chip detected. Wi-Fi firmware loads, scan works, access point works (a phone joined and was reachable). Left: joining an existing network; touch calibration.
- [ ] Step 2: interface skeleton, `nocry-fbd` over TCP with pairing, mDNS, nocry over the LAN. Done: pairing (SPAKE2) and the encrypted link, `nocry-netd` on the board, discovery (`nocry discover`), "Connect over the network…" in the window. Left: a run over the board's Wi-Fi rather than the USB tunnel (first try from a Mac under way), the touch interface with network setup, model and serial tail in the announcement.
- [ ] **Rescue Wi-Fi speed:** as its own access point the board sent at 5.5 Mbit/s with many failed transmissions; as a client of a home network it reached 1.5 to 2.8 MB/s. Find out why access-point mode is so much worse (channel 6 next to a strong router? the access point settings?).
- [ ] **Work the board can do alone should stay on the board.** Slot copy (`nocry-copy`) and every zero fill (`nocry-zero`: partition erase, erase all, factory reset, blank parts of an install) do since `nocry-fbd` 0.4; tested on a PC, not yet on hardware. Still through the computer: bringing back the boot logo kept on the board (a few MiB).
- [ ] **NebulaOS on hardware:** nocry lists and installs NebulaOS (`docs/nebulaos.md`), tested on a disk image only. Start it on a board from a nocry install, with the factory loader and with nocryboot. Later: a start trial that works with NebulaOS (it has no settings overlay for nocry's report script).
- [ ] **Board images from CI:** workflow `board-images` builds all of them from a fresh clone, nocry-rescue included (first complete run 2026-11-27), and `tools/get-images.sh` downloads the published set. Open: confirm that a run replaces the files of the `board-images` pre-release by itself (the first one could not create the release and it was made by hand); let the release workflow reuse this and ship nocry-rescue.
- [ ] **Board images on macOS:** `tools/build-images.sh` builds everything from a clone; verified on Linux only. Try it on a Mac with Docker Desktop (emulated containers, case-sensitive work volume) and fix what breaks.
- [ ] **Rescue joins a Wi-Fi network:** works on the Nebula from the set-up page (2026-11-27), and nocry on a PC found and read the board over the home network. Still to try: a saved network of the installed firmware; the join from nocry ("Board's Wi-Fi…") and finding the board again; the fallback after a wrong password on the fixed image; whether the driver setting or a mistyped password explained the first failures; speed at a realistic distance from the router.
- [ ] **Rescue web pages** (pairing code once, Wi-Fi, slots, copy, resets, jobs with progress): work on the Nebula. Next, in this order:
  - [ ] **Install firmware, boot loader, boot logo and full backup from the page:** written; nocry itself runs on the board for it (built for mipsel, staged unpacking so it fits in memory). Tested on a PC and under an emulator; needs its first run on the Nebula: a download and install, an upload, the loader and logo pages, a stop in the middle.
  - [ ] the rescue functions below, on the pages too
- [ ] **Rescue functions in the window.** The web pages and the command line (`nocry board ...`, `nocry install`) have them; the window still needs its dialogs for: storage health, file system check, the file browser, saving logs and configuration, restart, saving the Wi-Fi network for the firmware, and "remove system modifications" as its own clearly named action. None of the `nocry board` commands has run against a real board yet.
- [ ] **Still missing everywhere:** the kernel log of the last normal start; restart into the other slot once or into USB boot mode; a screen and touch test; changing a board's printer type from the web pages.
- [ ] **Rescue by flash disk** (for a board with a broken screen): a stick with a marker file, and later commands and Wi-Fi details on it. Parked 2026-11-27. What is known: the U-Boot tree has USB host, storage and FAT for this chip and they build (the experiment is in the git history, commit f39b17f, `tools/nocryboot-build/stage2/`); on the Nebula Pad the A sockets see nothing while the board is powered through its USB-C socket, in NebulaOS as in the experiment, so it can only be tried on a board powered through its 10-pin ribbon.
- [ ] **Step 3: rescue stored on the board, started by holding the screen.** Written: nocryboot 0.2 reads the touch chip and starts the rescue kernel from the runs noted in `ota` (emulator-tested), the rescue kernel unpacks itself, rescue asks for a five-second hold, `nocry rescue --install` stores it. To do on the Nebula: start the new rescue image from memory; store it; try the new loader from memory with the screen held (`nocry run-spl`); then install the loader. Later: a one-time "next start: rescue" flag; the touch interface itself.
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

- [x] **Released v0.1.0-alpha.4** (2026-11-27): nocryboot, loader installer, firmware on trial, nocry boot logo, K1 family. The packages themselves have not been run on a board.

- [x] nocryboot SPL: built from public source, matched against Creality's in the emulator, installed on both test boards (2026-11-26).
- [x] Recovery door, whole-slot fallback, count of starts; all exercised on the K1 (2026-11-27).
- [x] Loader installer in the CLI and GUI; first used through the GUI on the Nebula (2026-11-27).
- [x] nocry boot logo as an install option (default on the Nebula family) and switchable later; set on the Nebula (2026-11-27).
- [x] Copy to clipboard in the serial number and MAC dialog (2026-11-27).
