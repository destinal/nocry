# `sn_mac` partition (X2000E Creality boards)

Sources:
- the user's retail Nebula Pad, firmware `ota_version=1.1.0.23` (`ota_board_name=NEBULA`, built 2023-10-20)
- its rootfs scripts `/usr/bin/get_sn_mac.sh` and `clear_sn_mac.sh`, and `/etc/init.d/*`

## Format

GPT partition `sn_mac`: 1 MiB at offset 0x200000 (`/dev/mmcblk0p2`). **The record is plain ASCII with no checksum, signature or encryption.** It's NUL-terminated and the rest of the partition is zero. The firmware reads only the first 512 bytes.

```
SN;MAC;MODEL;BOARD;PCBA_TEST;;;
```

There are exactly 8 `;` separators, and `clear_sn_mac.sh` refuses anything else. The fields are:

| # | Field | Format, from `get_sn_mac.sh` | Retail Nebula Pad (redacted) |
|---|---|---|---|
| 1 | SN | exactly 14 hex digits; invalid → `00000000000000` | `112233CC001234` (last 8 digits = last 4 MAC bytes) |
| 2 | MAC | exactly 12 hex digits; invalid → fallback chain (below) | `AABBCC001234` |
| 3 | model (上位机, "host machine") | free text | `Nebula Pad` |
| 4 | board (主板型号, "mainboard model") | free text | `NEBULA V1.0.0.1` |
| 5 | PCBA factory-test flag | `1` = factory test mode | empty |
| 6–8 | reserved | | empty |

**MAC fallback** when the field is invalid: `/usr/data/macaddr.txt`, then `d03110` + eFuse CHIP_ID, then `d03110` + random.

By contrast, the 2025 K1C (X2600E) encrypts and signs `sn_mac` and `/bin/seed.sh` verifies it; none of that applies here.

## Consumers (firmware 1.1.0.23)

| Where | Uses | Effect |
|---|---|---|
| `S01hostname` | mac, model | hostname |
| `S11jpeg_display_shell` | model | boot logo orientation (`Nebula Pad` → `_portrait_rot270_`) |
| `S55klipper_service` | model, board, pcba_test | Klipper config set: `Nebula Pad` → `/usr/share/klipper/config/Nebula/Ender-3 Pro`, anything else → none. `pcba_test=1` → factory_printer.cfg |
| `S13mcu_update` | model, board | MCU firmware auto-update. Only K1 boards `CR4CU220812S10/S11/S12` with `CR-K1` / `CR-K1 Max` |
| `S97webrtc`, `S99mdns` | sn | device number, mDNS `_Creality-<SN>._udp` |
| `master-server` (binary) | model | knows `Nebula Pad`, `CR-K1`, `CR-K1 Max`, `F003`, `F005` |
| `display-server` (binary) | model | name table: **`F003` → `CR-10 SE`**, **`F005` → `Ender-3 V3 KE`**, `CR-K1`, `CR-K1 Max`, `Ender-3 V3 SE` |

## Confirmed from the KE firmware (`Ender-3_V3_KE_F005_ota_img_V1.1.0.17.img`, built 2025-07-29)

Extracted with `~/my-projects/extract-k1-ota/extract_creality_ota.py`. The OTA is v1 (7z with password seed `F005`) and contains `xImage` (kernel, 4.0 MB), `rootfs.squashfs` (115 MB) and `zero.bin` (rtos, 432 KB). Its `/etc/ota_info` has `ota_board_name=F005`.

- **The KE model is `F005`.** `S55klipper_service` installs the Klipper config only for `F005`.
- **The KE board string is `NEBULA V1.0.0.1`, the same as the retail pad.** `S13mcu_update` handles `board="NEBULA V1.0.0.1"` together with `model="F005"` by flashing the KE printer MCU on `/dev/ttyS1` from `fw_dir=F005`. Same hardware; only `model` differs.
- **The KE firmware extends the record:**
  `SN;MAC;MODEL;BOARD;PCBA_TEST;MACHINE_SN;STRUCTURE_VERSION;IC_LC`
  (整机SN = whole-machine SN; 结构版本 = structure version). `structure_version` 0 or empty selects config `F005`; 1 selects `F005_1`, which isn't present in 1.1.0.17. The retail record's empty fields 6–8 therefore work as-is.
- The KE rootfs still ships `config/Nebula/*` (the retail kit configs), but no script maps `Nebula Pad` to them, so a retail record on KE firmware gets no printer config.

## Which printers use the Nebula board

Determined from each firmware's own `S13mcu_update` board/model cases (2026-11-25). The firmware images come from `~/my-projects/crealityOS-firmwares`.

| Model code | Printer | Firmware checked | Board string | Same board as the retail pad? |
|---|---|---|---|---|
| `Nebula Pad` | Nebula Pad / Nebula Smart Kit (retail add-on) | user's pad, 1.1.0.23 (`ota_board_name=NEBULA`) | `NEBULA V1.0.0.1` | — (this is it) |
| `F003` | CR-10 SE | `CR-10_SE_F003_ota_img_V1.1.0.28` | `NEBULA V1.0.0.1` | **yes** |
| `F004` | Ender-5 Max | `F004_ota_img_V1.2.0.21` (also handles F003) | `NEBULA V1.0.0.1` | **yes** |
| `F005` | Ender-3 V3 KE | `Ender-3_V3_KE_F005_ota_img_V1.1.0.17` | `NEBULA V1.0.0.1` | **yes** |
| `F001` / `F002` | Ender-3 V3 / probably V3 Plus | `F001_ota_img_V1.2.3.28` | `CR4CU220812S10/11/12` (K1 mainboard) | no: a K1-class X2000E board |
| — | Ender-3 V4, Creality Hi (`CR4NU200360C20`) | `…_V1.1.0.65` | SWUpdate, "T113 Project" | no: **Allwinner T113**, not Ingenic |

- The retail kit's compatibility list (Ender-3 / Pro / V2 / V2 Neo / V3 SE / S1, with retailers disagreeing) names the printers the *same* retail pad attaches to. It doesn't list different boards.
- In the CR-10 SE, Ender-5 Max and Ender-3 V3 KE, the Nebula board is the printer's built-in head unit. Converting between these identities means changing `model` and installing that printer's firmware; the hardware is the same.

## Retail ↔ KE-bundled Nebula Pad: what we know

- The KE-bundled pad very likely carries model **`F005`**. That code maps to `Ender-3 V3 KE` in display-server. **This is inferred, not confirmed:** a KE pad's `sn_mac` dump would settle it, along with its `board` string.
- The KE ships its own firmware build. Changing `model` alone gives a retail firmware that half-recognizes F005: the display name changes, but `S55klipper_service` would install **no** Klipper config for an unknown model. A complete conversion therefore probably means flashing the KE firmware's rootfs/kernel as well, which the read-write ums mode can do, plus `model=F005`. The reverse conversion needs the retail firmware.
- K1 ↔ K1 Max on the K1 mainboard is the same mechanism: model `CR-K1` vs `CR-K1 Max`, with board `CR4CU220812S1x` choosing the config dir (`K1_CR4CU220812S12` vs `K1_MAX_CR4CU220812S12`) and the MCU firmware.
- **Keep SN and MAC unique per unit.** Copying another unit's record gives duplicate MACs and cloud identities. Only change `model` (and `board` if needed).

## Open

- [x] ~~Confirm `F005` and the board string~~: confirmed from KE firmware scripts.
- [x] ~~Compare KE init scripts with retail~~: done; see above.
- [ ] Work out how Creality's updater writes an OTA: which A/B slot, how `ota` is set, what happens to `rtos`, and whether `rootfs_data` is reset. Needed before flashing KE firmware over USB mass storage.
- [ ] Check whether `upgrade-server` refuses updates when the model and image don't match (it would block the "flash KE firmware" step via OTA, but not via ums).
