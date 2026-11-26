# nocry

A recovery, backup and firmware-installation tool for Ingenic X2000E-based Creality boards. It replaces Ingenic's Cloner with something safer and easier: everything it loads onto the board runs from RAM, every write is verified, and it cannot send eFuse or secure-boot commands.

**Status: early, tested on one board family.** Everything below has been run on a retail Creality Nebula Pad (and the same board converted to Ender-3 V3 KE firmware and back). K1 and K1 Max support is planned and untested. The GUI builds and runs on Linux only so far. Use it at your own risk and take a backup first.

![The nocry window: board details and the main actions](docs/screenshots/home.png)

<sub>The serial number and MAC address in the screenshots are made up. More: [partition view](docs/screenshots/partitions.png).</sub>

## What it does

- Loads its own SPL and second stage through the SoC's USB boot mode (hold BOOT, tap RESET). No blobs: both are built from GPL U-Boot source with the patches in `tools/burner-build`.
- Boots a small recovery Linux from RAM (`tools/recovery-build`) that offers a fastboot service, adb and a USB serial console. The PC never opens a disk device, so no raw-disk privileges are needed.
- **Back up** the whole eMMC to a file, verified by hashes computed on the board (about 5 minutes for 7.3 GiB).
- **Restore** a backup; only the parts that differ are written.
- **Install firmware** from a Creality `.ingenic` package or an unpacked OTA: repair in place (keep data) or clean install. Handles a completely blank board, generating a serial number and MAC address if needed.
- **Convert** between Nebula-based models (retail Nebula Pad, Ender-3 V3 KE, CR-10 SE, Ender-5 Max) by installing the other model's firmware.
- **Switch the active firmware slot** (A/B) and **factory reset**.

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

On Linux, install `host/contrib/60-nocry.rules` into `/etc/udev/rules.d/` so a normal user can open the USB devices. See `host/README.md` for the command-line tool and `gui/README.md` for the GUI.

## Safety

- The bootrom loader only sends requests 0x00–0x05; the stages are built without eFuse code.
- Destructive actions are dry runs or ask for confirmation first, and save the partition table, slot record and identity to the PC before writing.
- Boards with secure boot (the 2025 K1C, X2600E) are out of scope.
- Changing a board's serial number or MAC address, or copying another board's identity, is possible and is your responsibility.

## Credits and licence

Built on the work of [ballaswag/ingenic-usbboot](https://github.com/ballaswag/ingenic-usbboot), [k1-debian](https://github.com/k1-debian) (U-Boot and Linux trees for these boards) and [Ingenic-community](https://github.com/Ingenic-community). Not affiliated with Creality or Ingenic.

GPL-2.0-or-later. See `LICENSE`.
