# nocry (host tool)

Layout: a Cargo workspace. `core/` is the `nocry-core` library (USB boot, fastboot client, images, GPT, sn_mac, restore/backup engines) shared by the front ends; `cli/` is the `nocry` command. The binary is still `target/release/nocry`.

`nocry` loads a stage1 image (parameter header + SPL) and a stage2 into an Ingenic X2000(E) board through its bootrom's USB boot mode. Rust, using libusb via `rusb`.

```sh
cargo build --release
sudo cp contrib/60-nocry.rules /etc/udev/rules.d/ && sudo udevadm control --reload   # then replug
ln -s "$PWD/target/release/nocry" ~/.local/bin/nocry    # optional; images are still found

nocry list                       # boards, running stage2s, which images would be used
nocry ums --ro                   # eMMC as read-only USB disk (for backups)
nocry restore --os FIRMWARE [--repair] [--yes]
nocry snmac [--set-model F005 --yes]
nocry fastboot                   # then use the stock `fastboot` tool (below)
nocry recovery [--ums ro|rw]     # recovery Linux from RAM: adb + console (+ eMMC as USB disk)
nocry backup -o board.img        # full-disk image + board.img.sha256, verified by a second read
nocry write-image board.img [--yes]   # reimage from a backup: only differing chunks, verified
```

### Fast path vs. fallback (`--via auto|linux|uboot`)

`ums`, `restore`, `snmac`, `backup` and `write-image` reach the eMMC through whatever is already running. If nothing is running, nocry boots something, **preferably the recovery Linux** (HS200, 8-bit bus):

| | recovery Linux (`--via linux`, default when built) | U-Boot disk stage2 (`--via uboot`, fallback) |
|---|---|---|
| read | 39.9 MB/s (7.28 GiB backup: 196 s) | ~11 MB/s (~12 min) |
| write (verified) | ~34 MB/s (`restore --rewrite`: 10 s) | 8.4 MiB/s (48 s) |
| needs | `tools/recovery-build/build.sh` | `tools/burner-build/build.sh` only |

Verified on hardware (2026-11-25), both starting from USB boot mode:
- `nocry ums --ro` took the Linux path (fastboot, then recovery `nocry.ums=ro`): a read-only `/dev/sda` in 11.5 s.
- `nocry --via uboot ums --ro` booted the U-Boot read-only stage2: a read-only `/dev/sda` in 2.9 s, reading at 10.6 MB/s.

With `auto`, nocry falls back to U-Boot when no recovery image is built. Once a stage2 runs the bootrom is gone, so if the Linux path fails *after* booting, nocry says so: BOOT+RESET and rerun with `--via uboot`.

`write-image` refuses an image whose `sn_mac` belongs to another board, because it would clone that board's SN/MAC (`--allow-identity-change` overrides). To move firmware between boards, use `restore`, which keeps the board's identity.

### Recovery Linux

`nocry recovery` runs the whole sequence: it boots fastboot if needed, sends `tools/recovery-build/out/nocry-recovery.uImage` with nocry's own fastboot client (no android-tools required), waits for the board, and prints:

```
recovery up after 14s: 001:076 (port 1-7)
adb ready:  adb -s <SN> shell
console:    picocom /dev/nocry-console
```

With `--ums ro|rw`, the recovery also exports the eMMC as a USB disk. It reports the same SCSI identity as the U-Boot disk, so `nocry restore`, `nocry snmac` and `nocry ums` use it automatically, but it is driven by Linux at HS200 on the 8-bit bus. `--cmdline "..."` appends kernel arguments. Nothing is written to the eMMC unless you write to it.

### Fastboot

`nocry fastboot` boots the fastboot stage2 (`1209:0002`). After that the standard client works:

```sh
fastboot getvar all                      # model, serial, slots, partition sizes
fastboot oem gpt | fastboot oem snmac
fastboot fetch rootfs_a rootfs.img       # read back any partition (13.8 MiB/s)
fastboot flash rootfs_b rootfs.squashfs  # raw or sparse; bigger than 160 MiB is split by the client
fastboot erase rootfs_data
fastboot set_active b                    # Creality's ota record (kernel2/rootfs2/rtos2)
fastboot flashing unlock_critical        # needed for mmc0, gpt, bootloader, sn_mac (until power-off)
fastboot oem snmac-model F005
```

Names: every GPT partition, `kernel_a/_b`, `rootfs_a/_b`, `rtos_a/_b`, `bootloader` (SPL+U-Boot, LBA 34 to the first partition), `gpt` (LBA 0-33; flashing it also writes a matching backup GPT for this disk) and `mmc0` (whole eMMC). Every write is read back and compared.

**No separate boot step:** `ums`, `restore` and `snmac` work out the board's state themselves.

| Board state | What nocry does |
|---|---|
| nocry-ums stage2 already running | Uses its disk. A read-only stage2 can't switch modes, so a write (`--yes`) asks you to power-cycle into USB boot mode. |
| In USB boot mode (`a108:eaef`) | Boots `spl-x2000e.bin` plus the read-write stage2 (read-only for `ums --ro`), then uses the exported disk. |
| Neither | Explains how to enter USB boot mode. |

**Images** are found via `--images DIR`, then `$NOCRY_IMAGES`, then `<binary dir>/images`, then the project's `tools/burner-build/out` (located relative to the binary, symlinks resolved). An explicit `--target` (a block device or an image file) skips all of this. `nocry boot --spl … --stage2 …` still loads arbitrary images.

To enter USB boot mode: hold BOOT, tap RESET, release BOOT. The board then appears as `a108:eaef`.

## What `boot` does

1. Checks both images and refuses to send if there's a problem:
   - a raw SPL without a header
   - a cloner-generation header (warning only)
   - an over-size stage1
   - stage1 and stage2 swapped
   - an unknown stage2 (warning only)
2. Reads `GET_CPU_INFO`.
3. Stage 1: `SET_DATA_ADDR 0xb2401000`, `SET_DATA_LEN`, bulk OUT, then `PROG_STAGE1 0xb2401800`.
4. Polls `GET_CPU_INFO` until the SPL hands control back, with a 5 s limit. If the SPL never returns, the error names the likely causes: DDR parameters, a wrong image, or a secure-boot chip.
5. Stage 2: `SET_DATA_ADDR 0x80100000`, `SET_DATA_LEN`, bulk OUT, `FLUSH_CACHE`, then `PROG_STAGE2 0x80100000`.
6. Waits for the stage2 device: either the nocry-ums gadget `1209:0001`, or the burner re-enumerating. On Linux it prints the `/dev/sdX` that the eMMC is exported as.

## Safety

`nocry` only sends bootrom requests 0x00–0x05. `Bootrom::out()` refuses everything else, including the cloner requests that write eFuses and secure-boot settings, and a unit test enforces this. Loading code only touches RAM: if anything goes wrong, power-cycle and the board boots normally.

## Tests

`cargo test` runs the protocol sequence against a mock transport. The expected sequence is the same order ballaswag's `usbboot` uses successfully on K1 boards. The tests also cover partial bulk writes, an SPL that never returns, lost stage2 status, image checks and the allow-list.

## Firmware inputs

`--os` (and the GUI's Install firmware) accept:

| Input | Contains | Notes |
|---|---|---|
| Creality OTA update file (`*_ota_img_V*.img`) for an Ingenic-based printer | kernel, rootfs, rtos | Encrypted 7z; the password is derived from the product code or board id in the file name, with known ids tried as a fallback. Every image is md5-checked against the file's manifest. No bootloader inside, so it cannot bring up a blank board by itself. |
| `.ingenic` factory package | bootloader + partition table, kernel, rootfs, rtos | Can rebuild a blank board. |
| A folder with `xImage`, `rootfs.squashfs`, `zero.bin` | kernel, rootfs, rtos | E.g. from `extract_creality_ota.py`. |

Firmware for another mainboard is refused: the printer it is for is read from `etc/ota_info` inside the root filesystem, and only Nebula-family ids (NEBULA, F003, F004, F005) are accepted. K1-series and Ender-3 V3 (F001/F002, K1 mainboard) files open correctly but are rejected. Update files for Allwinner-based printers (K2, Ender-3 V4, Creality Hi) use a different container and are not read.

## Board images for a build from source

What nocry sends to a board (USB boot stages, the nocryboot loader, the boot logo program, the recovery Linux) is not in git: it is compiled from this repository. `tools/get-images.sh` downloads the set GitHub built from these sources into `images/` of the source tree; `tools/get-images.sh --build` compiles it instead (results under `tools/*/out`). nocry built from the tree finds either by itself. See "Building" in the top-level README.

For a copy of nocry that is not in its source tree, the same set can be unpacked into `images` in nocry's data folder (`~/Library/Application Support/nocry/images` on macOS, `%LOCALAPPDATA%\nocry\images` on Windows, `~/.local/share/nocry/images` on Linux). Installed releases carry the images already.
