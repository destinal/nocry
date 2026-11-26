# Restore, conversion and bare-metal provisioning (Nebula-family X2000E)

`nocry restore` does what Creality's factory Cloner policy does (`docs/ingenic-package.md`), but over nocry-ums instead of Ingenic's burner. It also handles the `sn_mac` identity.

## Workflow

```sh
# 1. Back up first (any state; read-only stage2)
nocry boot --spl spl-x2000e.bin --stage2 out/nocry_ums_ro_x2000/u-boot.bin
dd if=/dev/sdX of=backup.img bs=4M status=progress && sha256sum backup.img
#    power-cycle, re-enter USB boot

# 2. Read-write stage2
nocry boot --spl spl-x2000e.bin --stage2 out/nocry_ums_x2000/u-boot.bin

# 3. Dry run, then write
nocry restore --target /dev/sdX --os Ender-3_V3_KE_1.1.0.12.ingenic
nocry restore --target /dev/sdX --os Ender-3_V3_KE_1.1.0.12.ingenic --yes

# 4. Power-cycle. First boot formats rootfs_data/userdata, same as after a factory flash.
```

`--os` takes a `.ingenic` package or a directory from `extract_creality_ota.py`, which contains `xImage`, `rootfs.squashfs` and `zero.bin`. OTAs carry no boot area. A device that already boots keeps its own; a blank device needs `--boot <any Nebula-family .ingenic>`, since the boot area is identical across the family.

## What it writes

| Area | Action |
|---|---|
| boot area | Blank or unparseable GPT: the whole `u-boot-with-spl-mbr-gpt.bin` with **fresh random GPT GUIDs** (`--factory-guids` keeps the package's). Same layout already present: from 0x4400 only (SPL+U-Boot), so the device's GPT is kept. A different layout is refused. |
| `rtos`, `kernel`, `rootfs` | Package images, by GPT name. `rtos`/`kernel` remainders are zeroed. |
| `ota`, `rtos2`, `kernel2` | Zeroed. Boots slot A, like a factory flash. |
| `rootfs2` | First MiB zeroed (the squashfs superblock). |
| `rootfs_data`, `userdata` | **Quick wipe** (default): the first MiB plus the superblock slot of **every block group for every ext block size**. `--wipe full` zeroes everything (slow). `--keep-userdata` skips `userdata`. |
| `sn_mac` | See below. |

Every write is flushed, the page cache dropped, then read back and compared.

### Why the quick wipe covers every group

The board mounts both data partitions through `mount_mmc_ext4.sh`, which runs `fsck -y` and only then, if mount fails, runs `mke2fs`. e2fsck falls back to backup superblocks: it walks groups 1, 3, 5, 7, 9, 25, … when the primary is gone. A first version that zeroed only the group-1 backups **let e2fsck resurrect an old 4 KiB-block filesystem** in testing. The current wipe was verified against real e2fsck with:

| Case | Planted superblocks | After wipe | `e2fsck -y` |
|---|---|---|---|
| sparse_super, 1 KiB blocks (`rootfs_data` size) | 8 | 0 | rc=8, nothing recovered |
| sparse_super, 4 KiB blocks (`userdata` size) | 8 | 0 | rc=8, nothing recovered |
| non-sparse, 1 KiB blocks | 38 | 0 | rc=8, nothing recovered |
| non-sparse, 4 KiB blocks | 48 | 0 | rc=8, nothing recovered |
| the user's real retail Nebula backup | 8 + 8 | 0 + 0 | — |

## Identity (`sn_mac`)

- **Model:** from the firmware's `etc/ota_info` `ota_board_name` (`NEBULA` → `Nebula Pad`, `F003`/`F004`/`F005` unchanged). `--model` overrides it. A model that doesn't match the firmware is refused without `--force-model`, because the printer would get no Klipper config.
- **Board:** kept, or `NEBULA V1.0.0.1` if blank. `--board` overrides it.
- **SN/MAC:**
  - A valid existing pair is **kept**.
  - `--sn` + `--mac` set them explicitly, for example from the label.
  - A blank or invalid record, or `--new-identity`, generates a new pair:
    - MAC: a random **locally administered unicast** address (`x2`/`x6`/`xA`/`xE` first octet), so it can't collide with a vendor-assigned address.
    - SN: 6 hex digits (`--sn-prefix` or random) plus the last 8 hex digits of the MAC, the pattern factory units use.
  - Generated pairs are appended to `nocry-identities.log`.
  - Creality Cloud may not accept a made-up SN. Local and Klipper use don't care.
- A PCBA factory-test flag of `1` is cleared.

## Modes and skip-unchanged (2026-11-25)

**Every component is compared with the device in 1 MiB chunks** (page cache bypassed). Only differing chunks are written, then verified. The dry run lists each component as `ok … (unchanged)` or `WRITE … (n of m MiB differ)`. A second run of the same command is a no-op ("nothing to do").

| | full (default) | `--repair` |
|---|---|---|
| boot area | from `--boot`/`.ingenic`; GPT kept if layout matches | **kept if it looks valid** (GPT + SPL + U-Boot string); `--force-boot` to rewrite |
| rtos, kernel, rootfs | written (changed chunks only) | same |
| ota | cleared (slot A) | cleared **only if** it points elsewhere (e.g. `ota:kernel2`) |
| rtos2, kernel2, rootfs2 | cleared | **untouched** |
| rootfs_data (overlay) | wiped | **kept if the device already runs the same firmware** (board+version read from its own rootfs `etc/ota_info`), otherwise wiped; `--keep-overlay` / `--wipe-overlay` override |
| userdata | wiped (`--keep-userdata` to keep) | **kept** |
| sn_mac | kept if valid, generated if blank/invalid, model from firmware | same, but a model change is **refused** (that is a conversion; use full mode) |

`--rewrite` writes every planned chunk even when it already matches. Use it to exercise the write path without damaging anything, or to refresh flash. On hardware (2026-11-25): `--repair --rewrite --yes` with the pad's own firmware rewrote and verified 117.3 MiB in 48 s; a fresh comparison afterwards showed "nothing to do".

Repair is only as good as its validity checks: a boot area with a valid GPT and U-Boot string but corrupted code is not detected (use `--force-boot` when in doubt).

Offline tests on reflink copies of `backups/nebula-full-2.img`, with `backups/pkg-nebula-1.1.0.23/` (the pad's own firmware cut from the backup) and the KE `.ingenic`:

| Test | Result |
|---|---|
| repair, same firmware | 0 bytes to write, "nothing to do" |
| repair after damage (kernel 1 MiB, rootfs 2×1 MiB, rtos, `ota:kernel2`) | wrote exactly 5 MiB; image afterwards **bit-identical to `nebula-full-2.img`** |
| repair with KE firmware on a retail pad | refused ("conversion, not a repair") |
| full retail → KE conversion | 116.8 MiB written (was 142); rerun = nothing to do |
| bare metal from blank image | 114.3 MiB (already-blank regions skipped); rerun = nothing to do, no new identity |

## Full-disk backup and reimage (2026-11-25)

`nocry backup -o FILE` and `nocry write-image FILE` (see `host/README.md`) go through the recovery Linux by default.

Offline, on reflink copies of `nebula-full-2.img`:
- backup sha256 = the known sha256;
- 3 damaged 4 KiB spots → the dry run reports exactly 12 MiB (3 chunks), `--yes` writes and verifies them, the result is bit-identical, and a rerun says "nothing to do";
- an image with a different SN is refused (rc=1, nothing written) unless `--allow-identity-change`.

On hardware:
- `backups/nebula-full-3.img` was taken in 196 s at 39.9 MB/s, with a verified second read;
- versus `nebula-full-2.img`, every firmware partition is identical; only `rootfs_data` and `userdata` differ, written by a stock boot in between.

## Safety checks

| Check | Behaviour |
|---|---|
| Default mode | Dry run. Nothing is written without `--yes`. |
| Unknown block device | Refused unless sysfs model is `nocry eMMC`. Override: `--allow-any-device`. |
| Read-only device | Refused (the `nocry_ums_ro` build). |
| Partition instead of whole disk | Refused. |
| Mounted partitions | Refused; desktop automount can mount `rootfs_data`/`userdata`. |

## Tested (offline, 2026-11-25)

- **Bare metal:** a blank 15273600-sector image provisioned from `Ender-3_V3_KE_1.1.0.12.ingenic`.
  - `sfdisk` reads the fresh GPT. Its warnings about the backup GPT match factory boards.
  - Boot area (from 0x4400), `rtos`, `kernel` and `rootfs` are byte-identical to the package.
  - A generated identity was written and logged.
- **Second run on the same image:** GPT kept, identity kept, data partitions wiped (resurrection tests above).
- **Retail → KE conversion** on a reflink copy of the user's backup: SN/MAC/board kept, `model` `Nebula Pad` → `F005`, both data filesystems gone.

On hardware (2026-11-25): the read-write stage2 is now `nocry_ums_x2000_v12` (k1-debian V12 tree; the old uboot-xburst `nocry_ums_x2000` is legacy). It is validated: write and verify at 8.4 MiB/s, `nocry snmac` write verified, device bit-identical afterwards. A real `nocry restore` on hardware has not been run yet. Use `--target /dev/sdX` with the V12 read-write stage2.
