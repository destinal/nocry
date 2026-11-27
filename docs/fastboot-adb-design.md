# Fastboot and ADB for nocry: research and design

Status (2026-11-25):
- **Fastboot stage2 is implemented and validated on hardware** (`nocry_fb_x2000_v12`, `drivers/usb/gadget/f_nocry_fb.c` from `patches/k1deb/0002`).
- **The recovery Linux boots from RAM with working ADB** (`tools/recovery-build/`). On hardware: `adb shell` gives a root shell, `adb push`/`pull` round-trips byte-identical, the eMMC runs at HS200 8-bit, and the ACM console is `/dev/nocry-console`. The final image (loopback fix built in) has not yet been booted fresh; the fix was verified live.

## Recovery Linux (`tools/recovery-build/`)

```sh
tools/recovery-build/build.sh             # Docker; ~15 min first time, ~1 min after
nocry fastboot && fastboot boot tools/recovery-build/out/nocry-recovery.uImage
adb shell                                  # ~15 s later; or: picocom /dev/nocry-console
fastboot boot … --cmdline nocry.ums=ro     # also export the eMMC as a USB disk (rw: nocry.ums=rw)
```

- **Kernel:** k1-debian/linux `master-k1` (Linux 6.6.18, Ingenic X2000) + `patches/0001`:
  - `nocry_nebula.dts`: UART2, MSC0 8-bit HS200, OTG peripheral, watchdog and eFuse disabled.
  - The `nocrymem` earlycon.
  - Applied on top of `k1_defconfig` by `kernel-config.sh`.
- **Image:** a plain gzip uImage at `0x80010000`. The vendor `xImage` self-decompressor at `0x80F00000` would be overwritten by a kernel with an 18.9 MB initramfs.
- **Userland:** Buildroot 2025.02.20, soft-float glibc, Android 4.2 `adbd` (android-tools) over FunctionFS, busybox, e2fsprogs, util-linux, squashfs-tools. `S30nocry-usb` builds a configfs gadget `1209:0003` (ADB + ACM, optional mass storage), uses the SN as the USB serial, and logs to `/dev/kmsg`.
- **Debugging without a UART:**
  - `earlycon=nocrymem` writes every kernel message uncached into a ring at `0x0E800000` (`mem=232M` keeps Linux out of it).
  - After BOOT+RESET (not a power cycle), `nocry fastboot && fastboot oem dmesg` prints it.
  - `fastboot fetch ram ram.bin` dumps all 256 MiB at 28 MiB/s.

### Fast eMMC through the recovery (`nocry recovery --ums ro|rw`)

Linux drives the eMMC at HS200 on the 8-bit bus. The gadget exports it with the same `nocry eMMC` SCSI identity as the U-Boot disk, so `restore`, `snmac` and `ums` pick it up.

| | U-Boot nocry-ums | recovery `--ums` |
|---|---|---|
| sequential read (1 MiB direct) | ~11 MB/s | **39.8 MB/s** |
| sequential write (1 MiB direct, fsync) | 8.4 MiB/s | **33.7–34.6 MB/s** |
| `restore --repair --rewrite --yes` (117.3 MiB written and verified) | 48 s | **10.3 s** |

Verified on hardware (2026-11-25):
- ota, sn_mac, rtos, kernel, rootfs and rootfs2 (1014 MiB) read through it were identical to `backups/nebula-full-2.img`.
- 256 MiB of random data written to rootfs2 read back identical.
- rootfs2 was then restored from the backup and is bit-identical.
- `--ums ro` is really write-protected (host `ro=1`, "Write Protect is on"), and `restore --yes` refuses it.

### Fastboot service in the recovery (`nocry-fbd`, 2026-11-25)

`tools/recovery-build/fbd/nocry-fbd.c` runs over FunctionFS next to adb and the console (same gadget, `1209:0003`). It speaks standard fastboot plus two streaming extensions: `nocry-write:<name>:<off>:<size>` and `nocry-hash:<sha256|xxh64>:<name>:<off>:<size>:<chunk>`. From 0.4 also `nocry-copy:<name>:<src off>:<dst off>:<size>`: a copy inside the eMMC done by the board (identical megabytes skipped, written ones read back), answered with the bytes written; nocry uses it to copy a slot, so the firmware does not travel to the computer and back. No disk device is opened on the PC. Host test: `tests/fbd_stdio_test.py` (29 checks against a backup copy).

Measured on hardware with `nocry fb …` (nocry's own client):

| Operation | nocry-fbd | recovery USB disk | U-Boot disk |
|---|---|---|---|
| read 500 MiB (identical to backup) | 36.5 MB/s | 39.9 MB/s | ~11 MB/s |
| write 256–500 MiB | **40.4 MB/s** | 34 MB/s | 8.4 MiB/s |
| verify a write | **82 MB/s** (on-board xxh64) | ~40 MB/s (read back) | ~11 MB/s |
| "what differs?" compare | **84 MB/s** (xxh64), 28 MB/s (sha256) | ~40 MB/s | ~11 MB/s |

xxh64 runs at the eMMC's own read speed; sha256 is CPU-bound on the board. After the test rootfs2 is all zeros again, as in the backup.

`backup`, `restore`, `write-image` and `snmac` use this service by default when the recovery runs (`restore::Target` has a fastboot backend; `--target` or `--via uboot` select the disk paths). On hardware: `restore --repair --rewrite --yes` 10.8 s; whole-disk compare against a backup 100 s (7457 MiB, 0 differ); full backup 220 s at 35.6 MB/s plus 91 s on-board verify (5 min 11 s total; USB disk path 6 min 32 s), sha256 equal to `nebula-full-3.img`. The first version read at 27 MB/s because the host hashed and wrote the file on the thread that reads USB; those now run on a second thread. 36 MB/s is the raw fetch speed: the board is 68% idle and reads its eMMC at 82 MB/s, so the high-speed USB link is the limit (the mass-storage gadget reaches 39.9 MB/s).

### Pitfalls found on hardware

| Symptom | Cause | Fix |
|---|---|---|
| Fastboot stage2 never enumerates (278,840 B) | stage2 images above about 0x44000 bytes don't start (278,168 B worked) | `#undef CONFIG_SYS_LONGHELP` in NOCRY_FB → 272,376 B. Keep stage2 under 0x44000. |
| `bootm` would refuse the recovery image | U-Boot's default `CONFIG_SYS_BOOTM_LEN` is 8 MB; the kernel unpacks to 18.9 MB | `CONFIG_SYS_BOOTM_LEN 0x03c00000` in NOCRY_FB |
| Log empty, RAM full of stock-firmware data | `PANIC_TIMEOUT=10` + Ingenic watchdog → reset → bootrom boots the **stock firmware**, which overwrites all of DDR | `panic=0`, watchdog node disabled, `INGENIC_WDT` off |
| All binaries: `error -8`, `modprobe binfmt-464c` | XBurst2 FPU is NaN-2008 only (stock userland uses `ld-linux-mipsn8`); kernel refuses legacy-NaN ELF | Userland is now MIPS32r5 hard-float, FPXX, **NaN-2008** (`ld-linux-mipsn8.so.1`, like stock), and `ieee754=relaxed` is gone. Verified on hardware. (Interim fix was soft-float + `ieee754=relaxed`.) |
| nocry-fbd failed at exactly 2 GiB | 32-bit `off_t` in the MIPS userland; past 4 GiB a write would have wrapped to a wrong offset | `#define _FILE_OFFSET_BITS 64` plus a `_Static_assert` on `sizeof(off_t)` |
| `--ums ro` disk came up writable (host `ro=0`) | f_mass_storage refuses to change `lun.0/ro` once `lun.0/file` is set (EBUSY); the script set the file first | set `ro`/`removable`/identity before `file`; script verifies the value it reads back; `nocry recovery --ums ro` aborts unless the host sees the disk read-only |
| `mkdir functions/ffs.adb: Device or resource busy` | `CONFIG_USB_FUNCTIONFS` is the *legacy* built-in g_ffs gadget and claims FunctionFS | disable all legacy gadgets; only `USB_CONFIGFS_F_FS` |
| `udc …: failed to start nocry: -19` | adbd not running → FunctionFS function not ready → whole bind fails | script drops ADB and binds the console anyway |
| `adbd: cannot bind 'tcp:5037'`, adbd exits | 4.2 adbd (root, not secure) listens on 127.0.0.1:5037; `lo` comes up later (S40network) | `ifconfig lo 127.0.0.1 up` before adbd |

The stock kernel's built-in DTB (extracted from `xImage`) is the reference for Nebula pins:
- MSC0 8-bit on PD17–26, HS200, 1.8 V, 100 MHz;
- UART2 on PD30/31;
- stock cmdline `console=ttyS4,115200n8 init=/linuxrc root=/dev/mmcblk0p7` (`p8` for slot B).

## Hardware results (retail Nebula Pad, stock `fastboot` 37.0.0)

| Test | Result |
|---|---|
| `nocry fastboot` from USB boot mode | boots SPL + stage2, `fastboot devices` shows the board's SN |
| `getvar all`, `oem gpt`, `oem snmac`, `current-slot` | correct (model, partition table, slot a) |
| `fetch` gpt / sn_mac / kernel / rootfs_a (500 MiB, chunked by max-fetch-size) | all identical to `backups/nebula-full-2.img`; **13.8 MiB/s** |
| `flash` raw into rootfs_b | 6.2 MiB/s including read-back verify; rest of partition untouched |
| sparse flash with `-S 4M` (5 pieces: raw + zero + fill chunks) | correct |
| image > max-download-size (host auto-resparse) | 500 MiB restore of rootfs2 correct |
| `erase rootfs_b` (500 MiB) | 0.7 s (hardware ERASE on aligned groups), reads back all-zero |
| `set_active b` / restore | writes `ota:kernel2\n`; `current-slot` follows |
| `flash sn_mac` / `flash mmc0` while locked | refused ("critical"), data intact |
| oversize image (raw 9 MiB / sparse 600 MiB into 8 MiB kernel) | refused before writing |
| `flashing unlock_critical` + identical sn_mac rewrite, `lock_critical` | works; sn_mac identical |
| `fastboot flash kernel x` (no slot) | goes to `kernel_a` (current slot) |
| `fastboot boot` | **not yet tested** (needs the recovery kernel) |

After the tests rootfs2, ota and sn_mac were re-read and are bit-identical to the backup.

Erase: `mmc->erase_grp_size` group-aligned plain ERASE, zero-written edges; Ingenic's `mmc_berase()` is not used because it widens unaligned ranges and always issues SECURE_ERASE.

## What other platforms do

| Platform | Transport | How the PC controls the disk | Takeaway for nocry |
|---|---|---|---|
| **Android fastboot** (bootloader) | `getvar`, `download`, `flash`, `erase`, `boot`, `continue`, `reboot`, `set_active`, `flashing lock/unlock[_critical]`, `oem …` | By partition name. The host splits images larger than `max-download-size` into sparse chunks automatically (`-S` sets the chunk size). | The core everyone's tools already speak. Sparse support is mandatory here: 128 MiB RAM, 105 MiB rootfs. |
| **Android fastboot `fetch`** (Android 12+, libbootloader/GBL) | `fetch:<part>[:offset[:size]]` + `getvar:max-fetch-size`; the device sends the data back | Reads a partition, or any sub-range of a GPT partition or raw block device, back to the PC. The installed client (37.0.0) has `fastboot fetch PARTITION FILE`. | Gives fastboot backups, not just writes. |
| **fastbootd** (Android 10+, userspace) | adds `create/resize/delete-logical-partition`, `update-super`, `getvar is-userspace`, `is-logical:` | Repartitions dynamic partitions inside `super`. | Not applicable (no `super`), but shows the idea of a richer fastboot running in a recovery OS. |
| **Mainline U-Boot fastboot** | standard core plus extensions | Lookup by GPT name. `mmc0` = whole eMMC user area (`FASTBOOT_MMC_USER_SUPPORT`). `gpt` = write a GPT image. `fastboot_raw_partition_<name>=<offset> <size>` names arbitrary raw regions (SPL/U-Boot). `oem format` writes a GPT from `$partitions`. `oem run:<cmd>` runs any U-Boot command (`FASTBOOT_OEM_RUN`). `oem console` returns the console record buffer. | The closest model. Its naming scheme (`mmc0`, `gpt`, raw regions) gives full-disk control through an unmodified client. |
| **NXP uuu** (mfgtools) | fastboot + `UCmd:` (run a U-Boot command, wait) + `ACmd:` (async) | Scripts drive everything: `gpt write`, `mmc partconf`, raw writes. Kconfig warns it is "full access to any U-Boot command". | Scripted provisioning over fastboot. We already have this in `nocry restore`. |
| **barebox** | `usbgadget -A` fastboot, `-D` DFU, `-a` ACM, mass storage, **several at once in one composite gadget** | `fastboot.partitions` maps names to devices and files, including whole devices. `bbu-*` names run the bootloader-update handlers (validated writes). `oem exec`, `oem getenv/setenv`. | One USB device that is fastboot **plus** a serial console. Bootloader updates go through a validator, not raw writes. |
| **Rockchip rockusb** (also in U-Boot) | `rl`/`wl` LBA read/write, `rci` chip info, `wlx` partition write | Raw LBA access to the whole disk. | Raw-sector access, which `mmc0` plus offsets covers. |
| **Qualcomm EDL / Firehose** | XML: `program`, `read`, `erase`, `patch` | A host manifest (`rawprogram*.xml`) places images by sector. `patch*.xml` fixes GPT fields (disk size, CRCs) after writing. | Generic GPT images get their size-dependent fields patched for each disk. nocry's GPT handling already does this. |
| **Samsung Odin/Heimdall** | proprietary | PIT file = partition table. `download-pit`/`print-pit` read it, `--repartition` replaces it (wipes the device). | Reading the partition table must be a first-class operation, and repartitioning needs an explicit, scary flag. |
| **postmarketOS JumpDrive** / **Tow-Boot** | Linux initramfs: eMMC as USB mass storage + telnet over USB networking. Tow-Boot: hold volume-up for UMS mode. | Whole disk via UMS, plus a shell. | Exactly our ums stage2 plus the planned recovery Linux. Tow-Boot's mode depends on a working eMMC; ours is USB boot, so it works on a blank board. |

Sources: U-Boot `doc/android/fastboot.rst` and `drivers/fastboot/Kconfig`; AOSP fastbootd docs; U-Boot UUU patch (2021); barebox `user/usb.rst`; U-Boot `README.rockusb`; qdl/edl READMEs; heimdall(1); JumpDrive and pine64 docs.

## Findings on the Creality firmware (from `backups/pkg-nebula-1.1.0.23`)

- Kernel: Linux 4.4.94, Ingenic toolchain, gzip-compressed inside the uImage (at +0x4220). No IKCONFIG.
- Built in: DWC2 UDC, the gadget core, **configfs gadget**, **FunctionFS**, f_mass_storage.
- Missing: f_acm (no USB serial), and **initrd/initramfs support** (no initrd strings at all).
- **`/usr/bin/adbd` ships in the rootfs** (MIPS32r2, dynamically linked). It uses FunctionFS (`/dev/usb-ffs/adb/ep0..2`). Nothing in `/etc/init.d` starts it, so it's dead weight, but it works with this kernel.

So ADB on the stock kernel is just configfs and adbd. A RAM-only recovery needs our own kernel build, because the stock kernel can't take an initramfs.

## Proposed design

### Stage 2 `nocry_fb_x2000_v12`: fastboot in U-Boot

A compact fastboot function written for this 2013-era gadget API. It follows mainline U-Boot's command set and reuses the tree's `image-sparse.c`. Ingenic's `f_fastboot.c` isn't used: it relies on a hard-coded `partition_info[]` table and `run_command("mmc write …")`.

Partition names:

| Name | Region |
|---|---|
| any GPT name (`rtos`, `kernel`, `rootfs`, `rootfs_data`, `userdata`, `ota`, `sn_mac`, …) | that partition |
| `kernel_a/_b`, `rootfs_a/_b`, `rtos_a/_b` | Creality's A/B pairs (`kernel`/`kernel2`, …), so `fastboot --slot` works |
| `bootloader` | 0x4400 to the first partition (SPL + U-Boot; the GPT is kept) |
| `gpt` | primary GPT (LBA 0–33). Validated; the backup GPT is rebuilt for this disk's size |
| `mmc0` | the whole user area (offset 0, full size) |

Commands:
- `getvar`: `version`, `product` (from `sn_mac` model), `serialno` (sn), `max-download-size`, `max-fetch-size`, `partition-size:`, `partition-type:`, `has-slot:`, `current-slot`, `slot-count`, `is-userspace` = no, `secure` = no, `unlocked` = yes, `all`.
- `download`, `flash` (raw + sparse), `erase`, `fetch` (readback), `boot` (Android boot.img v0 or raw uImage from RAM, for the recovery Linux), `continue`, `reboot`, `reboot-bootloader`.
- `set_active:a|b`: writes the `ota` record. **The exact B-slot format needs confirming** from the firmware's OTA scripts before this is enabled.
- **`flashing unlock_critical`** is required, once per session (RAM only, gone after power-cycle), before writing `bootloader`, `gpt`, `mmc0` or `sn_mac`. It stops `fastboot flash mmc0 rootfs.img`-style typos. This is Android's lock_critical idea without any persistent state.
- `oem`:
  - `snmac` (show)
  - `snmac-model:<M>` (changes only the model field)
  - `console` (U-Boot console record buffer, for boards without a UART)
  - `gpt-print`
- **No eFuse code** (compiled out as in the ums builds). **No `oem run` in the default build:** arbitrary `mw` could reach the eFuse controller. A separate dev build may add it.

Max download/fetch size: 160 MiB at 0x84000000 (the board has **256 MiB** DDR; U-Boot relocates to the top). The client sparses anything larger automatically.

Host: `nocry fastboot` auto-boots this stage2, the same way `nocry ums` does, then prints `fastboot devices`. After that the stock `fastboot` tool does everything. USB ID: pid.codes test PID `1209:0002` until real PIDs are assigned. The fastboot client matches on interface class ff/42/03, not VID/PID.

### Later: composite gadget (barebox-style)

Fastboot + CDC-ACM U-Boot console in one stage2. ums stays separate: a host page cache over the same eMMC that fastboot is writing would go stale.

### Recovery Linux (ADB)

Two options:
1. **Our own Ingenic 4.4 kernel build** with initramfs + f_acm + FunctionFS. Booted RAM-only with `fastboot boot`. adbd + getty on ACM + optional UMS. Like JumpDrive, it never touches flash. Needs the kernel source (k1-debian or Ingenic's tree) and a build matching this board's DT.
2. **Stock kernel + a recovery squashfs in the unused B slot** (`kernel2`/`rootfs2`), with the stock adbd. No kernel build, but it writes flash and has no serial console.

Recommendation: option 1. It keeps the "RAM only, can't brick" property.
