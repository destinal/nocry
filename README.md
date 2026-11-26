# nocry

A recovery, backup and firmware-installation tool for Ingenic X2000E-based Creality boards. It replaces Ingenic's Cloner with something safer and easier: everything it loads onto the board runs from RAM, every write is verified, and it cannot send eFuse or secure-boot commands.

**Status: early, tested on one board family.** Everything below has been run on a retail Creality Nebula Pad (and the same board converted to Ender-3 V3 KE firmware and back). K1 and K1 Max support is planned and untested. Use it at your own risk and take a backup first.

## Download

Get the latest pre-release from the [releases page](https://github.com/destinal/nocry/releases).

| Platform | Package | State |
|---|---|---|
| Linux (x86-64) | `.AppImage` | Tested, including downloading firmware from Creality's list and installing it (in the development build; the list is not in the 0.1.0-alpha.1 packages yet). Install `60-nocry.rules` once so a normal user can open the board (see Building). |
| macOS (Apple silicon) | `.dmg`, drag to Applications | Tested: a full backup and a firmware install on a Nebula Pad both worked. No driver or setup needed. Unsigned: right-click the app and choose Open the first time. |
| Windows (x64) | `-setup.exe`, installs for the current user without administrator rights | Builds, **not yet tested**. The board in USB boot mode probably needs a WinUSB driver that the installer does not provide yet. |

![The nocry window: board details and the main actions](docs/screenshots/home.png)

<sub>The serial number and MAC address in the screenshots are made up. More: [Advanced tab](docs/screenshots/advanced.png).</sub>

## What it does

- Loads its own SPL and second stage through the SoC's USB boot mode (hold BOOT, tap RESET). No blobs: both are built from GPL U-Boot source with the patches in `tools/burner-build`.
- Boots a small recovery Linux from RAM (`tools/recovery-build`) that offers a fastboot service, adb and a USB serial console. The PC never opens a disk device, so no raw-disk privileges are needed.
- **Back up** the whole eMMC to a file, verified by hashes computed on the board (about 5 minutes for 7.3 GiB).
- **Restore** a backup; only the parts that differ are written.
- **Install firmware** from a Creality `.ingenic` package or an unpacked OTA: repair in place (keep data) or clean install. Handles a completely blank board, generating a serial number and MAC address if needed.
- **Convert** between Nebula-based models (retail Nebula Pad, Ender-3 V3 KE, CR-10 SE, Ender-5 Max) by installing the other model's firmware.
- **Switch the active firmware slot** (A/B) and **factory reset**.
- **Install to the other slot**: put a second firmware version of the same printer type beside the running one, then choose which starts. Untested on hardware beyond the write itself: no board has been started from slot B yet.
- **Copy a slot**: copy the firmware of slot A into slot B or the other way round, verified (about 40 seconds), from the Switch Active Slot window or with `nocry copy-slot a --yes`.
- **Secure erase**: a factory reset that also destroys the old settings and files, using the storage chip's secure erase where it has one (it does on the Nebula Pad), then checks that every byte is zero. About 3 minutes.
- **Board log**: read the recovery system's kernel log from the window or with `nocry boot-log`.
- **Rebuild a blank board** from an update file or the list: the bootloader and partition table that update files lack are fetched once from Creality's factory package and checksum-verified.
- **Advanced**: back up, restore or erase a single partition; edit the serial number, MAC address and model; erase the entire storage.
- **Pick firmware from a list**: versions published by Creality for the supported printers, downloaded and checksum-verified, or use a local update file (`*_ota_img_V*.img`), `.ingenic` package or folder.
- **Shell access**: the window shows whether `adb` and the USB serial console are available on the running recovery and gives the exact command for each.

## Layout

| Path | What |
|---|---|
| `host/core` | `nocry-core`, the Rust library: USB boot loader, fastboot client, image formats, GPT, `sn_mac`, backup/restore/install engines |
| `host/cli` | the `nocry` command-line tool |
| `host/ffi` | C interface to the library (`nocry.h`) |
| `gui` | Qt 6 Widgets front end |
| `tools/burner-build` | reproducible Docker build of the SPL and U-Boot stages (fastboot, USB disk) |
| `tools/recovery-build` | reproducible build of the recovery Linux (kernel patch, Buildroot config, `nocry-fbd`) |
| `tools/splhdr` | generator for the SPL parameter header (DDR settings) |
| `docs` | findings and design notes: USB boot protocol, `sn_mac`, `.ingenic` packages, restore logic, fastboot/recovery design, GUI plan |

## Building

```sh
tools/burner-build/build.sh        # SPL + stage2 images (needs Docker)
tools/recovery-build/build.sh      # recovery Linux (needs Docker; ~15 min the first time)
(cd host && cargo build --release) # the nocry CLI: host/target/release/nocry
cmake -S gui -B gui/build -DCMAKE_BUILD_TYPE=Release && cmake --build gui/build -j   # GUI (Qt 6)
```

On Linux a udev rule is needed so a normal user can open the USB devices. The window offers to install it on first run (one password prompt); by hand, copy `host/contrib/60-nocry.rules` into `/etc/udev/rules.d/`. See `host/README.md` for the command-line tool and `gui/README.md` for the GUI.

## Safety

- The bootrom loader only sends requests 0x00–0x05; the stages are built without eFuse code.
- Destructive actions are dry runs or ask for confirmation first, and save the partition table, slot record and identity to the PC before writing.
- Boards with secure boot (the 2025 K1C, X2600E) are out of scope. A board whose processor is not in the X2000 family is refused before anything is sent to it. Secure boot itself cannot be queried, so an X2000 board with it enabled shows up as a loader that never starts, and nocry says so.
- Changing a board's serial number or MAC address, or copying another board's identity, is possible and is your responsibility.

## Credits and licence

Built on the work of [ballaswag/ingenic-usbboot](https://github.com/ballaswag/ingenic-usbboot), [k1-debian](https://github.com/k1-debian) (U-Boot and Linux trees for these boards) and [Ingenic-community](https://github.com/Ingenic-community). Not affiliated with Creality or Ingenic.

GPL-2.0-or-later. See `LICENSE`.
