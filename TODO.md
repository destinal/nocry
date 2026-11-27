# To do

Open work on nocry, newest state first. Checked items are removed after a while; the history is in git.
Last updated: 2026-11-27.

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

- [ ] **Step 1: hardware bring-up from RAM on the Nebula Pad.** Done: screen, backlight, touch chip answers, Wi-Fi chip detected. Wi-Fi firmware loads, scan works, access point works (a phone joined and was reachable). Left: joining an existing network; touch calibration.
- [ ] Step 2: interface skeleton, `nocry-fbd` over TCP with pairing, mDNS, nocry over the LAN. Done: pairing (SPAKE2) and the encrypted link, `nocry-netd` on the board, discovery (`nocry discover`), "Connect over the network…" in the window. Left: a run over the board's Wi-Fi rather than the USB tunnel (first try from a Mac under way), the touch interface with network setup, model and serial tail in the announcement.
- [ ] **Rescue Wi-Fi speed:** as its own access point the board sent at 5.5 Mbit/s with many failed transmissions; as a client of a home network it reached 1.5 to 2.8 MB/s. Find out why access-point mode is so much worse (channel 6 next to a strong router? the access point settings?).
- [ ] **Work the board can do alone should stay on the board.** Slot copy (`nocry-copy`) and every zero fill (`nocry-zero`: partition erase, erase all, factory reset, blank parts of an install) do since `nocry-fbd` 0.4; tested on a PC, not yet on hardware. Still through the computer: bringing back the boot picture kept on the board (a few MiB).
- [ ] **NebulaOS on hardware:** nocry lists and installs NebulaOS (`docs/nebulaos.md`), tested on a disk image only. Start it on a board from a nocry install, with the factory loader and with nocryboot. Later: a start trial that works with NebulaOS (it has no settings overlay for nocry's report script).
- [ ] **Board images from CI:** workflow `board-images` builds all of them from a fresh clone, nocry-rescue included (first complete run 2026-11-27), and `tools/get-images.sh` downloads the published set. Open: confirm that a run replaces the files of the `board-images` pre-release by itself (the first one could not create the release and it was made by hand); let the release workflow reuse this and ship nocry-rescue.
- [ ] **Board images on macOS:** `tools/build-images.sh` builds everything from a clone; verified on Linux only. Try it on a Mac with Docker Desktop (emulated containers, case-sensitive work volume) and fix what breaks.
- [ ] **Rescue joins a Wi-Fi network:** works on the Nebula from the set-up page (2026-11-27), and nocry on a PC found and read the board over the home network. Still to try: a saved network of the installed firmware; the join from nocry ("Board's Wi-Fi…") and finding the board again; the fallback after a wrong password on the fixed image; whether the driver setting or a mistyped password explained the first failures; speed at a realistic distance from the router.
- [ ] **Rescue web pages** (pairing code once, Wi-Fi, slots, copy, resets, jobs with progress): work on the Nebula. Next, in this order:
  - [ ] **Install firmware from the page** (downloaded by the board, or uploaded from the browser). Needs nocry's install logic on the board rather than a second implementation: build the Rust core for the board's processor (mipsel is not a ready-made target) and make it stream firmware instead of holding it in memory (the board has about 230 MB of RAM); stage downloads on the data partition.
  - [ ] the rescue functions below, on the pages too
- [ ] **Rescue functions for both the desktop app and the web pages** (asked for 2026-11-27; what each has today in brackets):
  - [ ] Remove system modifications only [web: yes; desktop: factory reset can keep files, but not as its own clearly named action]
  - [ ] Check and repair the data file system [web: yes, not yet run on a board; desktop: no]
  - [ ] File browser for the data partition: browse, download, delete, upload [web: yes, writing not yet run on a board; desktop: no]
  - [ ] Save before wiping [web: configuration, logs, identity record; desktop: full and per-partition backup, no configuration-only download]
  - [ ] "Why didn't it start?" in one file [web: printer logs, rescue kernel log, status, the nocryboot note; desktop: board log only. Neither has the kernel log of the last normal start]
  - [ ] Storage health [web: yes; desktop: no]
  - [ ] Write the Wi-Fi network into the installed firmware's settings [web: yes, not yet run on a board; desktop: no]
  - [ ] Restart [web: into the firmware, not yet run on a board; neither: the other slot once, USB boot mode; desktop: none]
  - [ ] Screen and touch test [neither]
  - [ ] Identity: show and repair [desktop: yes; web: printer type and a download of the record]
  - [ ] Boot loader and boot picture [desktop: yes; web: no]
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

- [x] **Released v0.1.0-alpha.4** (2026-11-27): nocryboot, loader installer, firmware on trial, nocry boot picture, K1 family. The packages themselves have not been run on a board.

- [x] nocryboot SPL: built from public source, matched against Creality's in the emulator, installed on both test boards (2026-11-26).
- [x] Recovery door, whole-slot fallback, count of starts; all exercised on the K1 (2026-11-27).
- [x] Loader installer in the CLI and GUI; first used through the GUI on the Nebula (2026-11-27).
- [x] nocry boot picture as an install option (default on the Nebula family) and switchable later; set on the Nebula (2026-11-27).
- [x] Copy to clipboard in the serial number and MAC dialog (2026-11-27).
