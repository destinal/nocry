# nocry GUI: requirements and plan

Status: draft, 2026-11-25. Items marked **[open]** await a decision; the stated default applies until then.

## Goal

A recovery, installation and factory-reset tool for Ingenic-based Creality boards that ordinary printer owners can use safely, with extra control for power users. It replaces Ingenic's Cloner, which is hard to use and easy to damage a board with.

Platforms: macOS, Linux (AppImage), Windows. The GUI is the main interface; the CLI stays.

## Users

| User | Needs |
|---|---|
| Regular owner | "My printer is broken or messy: back it up, wipe it clean, install the firmware I pick, switch which system boots, factory reset." No jargon, no way to brick by accident. |
| Power user | Per-partition backup/restore, SN/MAC editing, model conversion, full erase, blank-board rebuild, shell and logs. |

## Features

### Simple mode (home screen)

| Action | What it does |
|---|---|
| Device status | Live: not connected / USB boot mode / working. Shows model, firmware version in slot A and B, active slot, SN. Per-board pictures show how to enter USB boot mode. |
| Back up | Whole board to one file with checksum, verified. |
| Install firmware | Pick from a list of Creality firmware for this device (downloaded and cached), or open an `.ingenic` or OTA file. Offers "keep my data" (repair) or "clean install". |
| Start over (clean reimage) | Wipe and reinstall a chosen firmware; identity (SN/MAC) is kept. |
| Switch system | Choose slot A or B to boot. |
| Factory reset | Erase only the data partitions the firmware recreates on first boot. |
| Restore a backup | Write a backup file back (only differing parts, verified). |

### Advanced mode (behind a switch, with warnings)

- Back up / restore / erase individual partitions.
- Edit SN and MAC; generate a new identity.
- Convert model (Nebula ↔ Nebula now; K1 family later). Warns that the wrong model means no matching printer config.
- Erase the entire disk.
- Rebuild a blank board: partition table, bootloader, RTOS, kernel, rootfs, `ota`, `sn_mac`. The bootloader and partition table come from the matching `.ingenic`, fetched automatically when the chosen image is an OTA.
- Shell (adb) and boot log viewer.

### Safety

- Before any write, save the first 3 MiB (partition table, `ota`, `sn_mac`) to the PC automatically.
- Every write is verified by read-back.
- Destructive actions show exactly what will be erased and need a typed or explicit confirmation; advanced ones add a "know what you're doing" warning.
- eFuse and secure-boot commands can never be sent (compiled out and allow-listed).
- Secure-boot boards (K1C 2025, X2600E) are detected and refused with an explanation.
- Writing another board's backup is refused unless explicitly allowed (it clones SN/MAC).
- Everything runs from RAM on the board; a failed operation can always be retried from USB boot mode.

## Architecture

```
nocry-core (Rust library)      device detection, bootrom loader, fastboot client,
                               images (.ingenic, OTA), GPT, sn_mac, plans, verification
  ├─ nocry      (CLI)
  └─ nocry-gui  (GUI)
On the board (RAM only):  SPL → fastboot stage2 (U-Boot) → recovery Linux with fastboot service
```

- **Transport: fastboot everywhere.** The recovery Linux runs a fastboot service (flash, fetch, erase, hash-range, slot and identity commands). The U-Boot stage speaks the same protocol and is the fallback. The PC never opens a disk, so no raw-disk permissions are needed. USB mass storage remains a CLI option.
- **Board-side hashing** makes "what differs?" fast: only changed chunks cross USB.
- **Board profiles:** each family (Nebula; later K1/K1 Max/K1C) is a data package: DDR settings, recovery device tree, partition layout, model names, pictures.
- **No external tools:** OTA extraction and squashfs reading are built in.

## Privileges

| Platform | Install and run | One-time exception |
|---|---|---|
| macOS | no admin | none expected |
| Linux AppImage | no admin | one udev rule (password prompt, once) |
| Windows | no admin; our stages self-describe so the built-in USB driver loads | the Ingenic bootrom device probably needs a one-time driver install. **To be tested first.** |

## Decisions

| Topic | Decision |
|---|---|
| Audience | Regular users first, power users via Advanced. Decided. |
| Data transport | fastboot service in recovery Linux; U-Boot fallback. Decided. |
| GUI toolkit | Qt 6 Widgets in C++ on top of `nocry-core` through a C interface (`host/ffi`). Chosen for the most native look on all three platforms; cost: C++ UI layer, larger bundles, per-platform Qt deployment. Decided. |
| Windows bootrom driver | **[open]** default: accept a one-time admin step if unavoidable. |
| Test hardware | Nebula Pad, K1 and K1 Max boards available. No K1C: pre-2025 K1C support would ship untested unless a board or a tester turns up. Decided. |
| Firmware distribution | **[open]** default: download from Creality at runtime, never bundle. |
| Code signing | **[open]** default: unsigned first releases (OS warnings). |
| Licence | **[open]** default: GPL, since GPL U-Boot/Linux binaries ship with it. |

## Unknowns to research

- Creality Cloud firmware list API (per-model listing, download URLs, stability).
- Whether the bootrom device works driverless on Windows.
- OTA container format details for a built-in extractor (the user's `extract_creality_ota.py` is the reference).
- K1-family DDR parameters, pins and partition layouts.
- Real USB IDs (pid.codes) to replace the test PIDs.

## Phases

Progress (2026-11-25), phase 1:
- Done: fastboot service in the recovery (`nocry-fbd`) with streaming write and on-board hashing; backup, restore, write-image and snmac run over it by default with no disk device on the PC; `host/` is a workspace with the `nocry-core` library and the `nocry` CLI.
- Done: device orchestration moved into `nocry-core` (`session`, `events`); C interface `host/ffi`; first Qt window (`gui/`) showing live state, device details and partitions, verified against the Nebula on Linux.
- Still to do in the GUI: every action (backup, install, switch system, factory reset, restore), cancellation, error dialogs; builds for macOS and Windows have not been attempted.

1. **Core split and transport:** `nocry-core` library; fastboot service in recovery with hash-range; measure speed; port restore/backup to it.
2. **Images:** built-in OTA and squashfs reading; Creality firmware list and download cache; auto-fetch of the `.ingenic` boot area.
3. **GUI, simple mode:** status, backup, install, start over, switch system, factory reset, restore.
4. **GUI, advanced mode:** partitions, identity, conversion, full erase, blank-board rebuild, shell/log.
5. **Platforms and packaging:** Windows driver story, macOS app, AppImage, udev helper.
6. **More boards:** K1, K1 Max, K1C (pre-2025).
