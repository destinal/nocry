# nocry-rescue: design

*A short introduction is in [`tools/recovery-build/README.md`](../tools/recovery-build/README.md); this file is the design notes and the bring-up log.*

Status: design, 2026-11-27. Nothing is built yet. Decisions marked **decided** were made with the project owner.

## Goal

A small Linux with a touchscreen interface that lives on the printer's own storage and can be started when the normal firmware cannot, or on purpose. It lets the owner repair or change the printer **without taking the board out or connecting a USB cable**:

- set up the network (Wi-Fi client with DHCP or a fixed address, or its own access point);
- be driven by nocry on a PC over the LAN, found automatically;
- on the touchscreen itself: choose the firmware slot, factory reset, start the printer normally; later, install firmware.

It needs nocryboot as the board's loader. USB boot mode stays the last resort behind it.

## Decisions

| Topic | Decision |
|---|---|
| Where it lives | **decided (changed 2026-11-27):** two copies, in the last 32 MiB of `rootfs` and of `rootfs2`, behind the system images. No repartitioning. Before: one file in `userdata`. |
| How the loader finds it | at those two fixed places; it checks the header and the contents against the CRCs in the uImage header before starting anything (`docs/nocryboot.md`) |
| Losing it | firmware updates, factory resets (Creality's and nocry's) and a formatted data partition leave the copies alone. A restore of a full backup, erasing the storage, or Ingenic's Cloner removes them; nocry stores them again. |
| Starting it | finger on the screen at power-on (the nocryboot loader reads the touch chip), nothing more to hold (a 3-second count was dropped again); automatically when no slot can be started; on request (a one-shot note in `ota`) |
| USB keyboard as trigger | not possible: the loader has no USB stack and about 1.6 KB of room |
| First version | **decided:** Nebula Pad first. Network setup, slot choice, factory reset, start normally, control from nocry over the LAN. |
| Firmware install from the touchscreen | **decided:** second step (needs nocry's install engine built for the board) |
| LAN access | proposed: pairing code shown on the touchscreen, typed into nocry on the PC; discovery by mDNS |
| Interface | **built:** `nocry-ui`, its own small program (GPL-2.0-or-later) on LVGL 8.3 (MIT), drawing into the frame buffer and reading the touch chip itself; every action is a `nocry-rescue-do` / `nocry-wifi` command, the same ones the web pages use |

## Where there is room

Rescue with a touch interface, Wi-Fi and its firmware is 17 MiB. The first partitions have no such room: `ota` and `sn_mac` are 1 MiB each, the `rtos` partitions 4 MiB each, and each `kernel` partition has about 4 MiB to spare.

The two root file system partitions are 500 MiB each and a system image takes 105 to 158 MiB of one, so over 340 MiB at the end of each is never written by the firmware or its updater. The last 32 MiB of each hold a copy of the rescue image, as raw sectors: the loader needs no file system and no pointer. The first design kept one file in `userdata` with a pointer in `ota`; it went with every factory reset.

## Starting it

The first stage has almost no room and no input devices. The boot logo program (`rtos/`) runs before Linux, owns the screen, and has room:

- It reads the touch controller. (As built, differently: the nocryboot loader reads the touch controller and starts the rescue system, which asks for a 3-second hold; see `docs/nocryboot.md`.) The original plan: a finger held down at power-on brings up "Start nocry-rescue? Keep holding / tap to confirm". Without a finger, nothing changes and no time is added.
- It reports the result to the first stage in the word it already uses for the panel id (a flag bit beside it).
- The first stage then loads rescue instead of the slot's kernel.

Also:

- **No startable slot:** today nocryboot restarts into USB boot mode. With rescue present it starts rescue first.
- **On request:** `nocryboot:rescue-once` in the `ota` notes, set by nocry or by a command on the running printer; the loader clears it when it acts on it.

Rescue is started with the display already switched on by the boot logo program, like the firmware's Linux is.

## The rescue system

Runs entirely from RAM (kernel with built-in initramfs, like today's recovery), so it can rewrite any partition, including the file it was loaded from.

Base: the existing recovery build (`tools/recovery-build`: Linux 6.6 from k1-debian, Buildroot, `nocry-fbd`). The kernel tree already carries what the hardware needs:

| Part | Nebula Pad | K1 family | In the kernel tree |
|---|---|---|---|
| Display | 480x272 RGB panel | 480x800, four panel types | Ingenic framebuffer driver; `creality_k1.dts` has the K1 panel |
| Touch | NS2009 resistive, I2C bus 4 | Goodix GT9xx, I2C bus 4 | GT9xx yes; NS2009 no (small driver, or read from user space) |
| Wi-Fi | BCM43438 (AP6212A) on MSC1, power PA1, enable PD4 | same chip, enable PE2 | brcmfmac; `creality_k1.dts` has it |
| Backlight | PWM PC0, power PC22 | – | PWM driver |

Added to the recovery's contents: the touch interface, wpa_supplicant and a DHCP client, a small DNS/DHCP server for access-point mode, an mDNS responder, the Wi-Fi firmware.

The NS2009 is resistive, so the interface needs a one-time calibration screen (stored in `rootfs_data`), and large touch targets.

## Control over the LAN

`nocry-fbd` already implements everything nocry needs (read, write, hash, erase, slots, files) over USB. It gets a second transport: the same protocol over TCP, the way fastboot does it. nocry's core gets a TCP transport beside the USB one and a discovery step.

- **Discovery:** rescue announces `_nocry._tcp` by mDNS with the model and the last digits of the serial number; nocry lists what it finds, and also accepts an address typed in.
- **Access:** rescue shows a short code on the touchscreen. nocry asks for it once per session; both sides derive a key from it and the connection is encrypted and authenticated with that key. Nobody can drive the printer without standing at it.
- Everything the window does over USB then works over the LAN: backup, restore, install, slots, factory reset, identity, loader, boot logo.

## Touchscreen functions, first version

- Network: scan and join a Wi-Fi network, DHCP or fixed address, or start an access point; show the address and the pairing code.
- Slots: what is in A and B, which starts, switch.
- Factory reset (and put rescue back afterwards).
- Start the printer normally; restart; power-off hint.
- Status: loader, firmware versions, why rescue was started (the loader's note).

Second step: install firmware from Creality's list or a USB stick on the printer itself.

### As built (`tools/recovery-build/ui/nocry-ui.c`)

- Home: eight tiles (Wi-Fi, Slots, Install firmware, USB drive, Advanced, System info, Eject drive, Reboot; Advanced holds Repair and Remote access), the network and address underneath, the pairing code top right on every screen. Eject drive is greyed out while no flash drive is mounted.
- Wi-Fi: scan, choose, type the password on an on-screen keyboard, join; a fixed address instead of DHCP ("Static IP": address, subnet mask 255.255.255.0 and DNS 8.8.8.8 filled in, gateway and DNS optional); disconnect from the network and stay off it; the board's own access point with a name and password of the owner's choosing (kept; an empty password makes it open; "nocry" / "nocry111" otherwise), a QR code a phone joins it from, and a way to turn it off; save the network for the installed firmware. Wi-Fi turned off lasts until a network is chosen, the access point is started, or the board restarts.
- Slots: what is in A and B, which one starts, switch, copy one over the other.
- Long work with a measurable size shows the speed and the time left for the step that is running, from the last twenty seconds.
- Install firmware: the list for this board (refreshed from the internet on request), into the starting slot or the other one; downloaded and checked first, then a summary to confirm.
- Repair: check either file system, storage health, remove system modifications, factory reset.
- Long work shows a progress bar and a Stop button that says what stopping leaves behind. Anything that erases asks first.
- USB drive (below): back up and restore the storage or single partitions, install a firmware file from the drive, eject.
- Touch calibration: three crosses, once; kept in the last 4 KiB of the room that holds the rescue image, with the turn and the eject setting, so it survives what the image survives. It is worked out for the panel, not for one way up. Holding a finger anywhere for ten seconds starts it again, so a wrong calibration cannot lock anyone out.
- Which way up: the panel is used as it is, wide (480x272 on the Nebula Pad), which is how most NebulaOS owners have theirs. A button with the usual rotate-right arrow turns the interface a quarter turn clockwise each time (one press from wide to the usual upright) (wide, upright, and both the other way round); it sits in one corner of the glass, top right when wide and bottom right when upright. The turn is kept, and the count at start and the text screens follow it. The upright keyboard has ten tall keys per row. `--turn 0|90|180|270` overrides.
- The pressure reading of the touch chip depends on where the finger is, not only on how hard it presses: towards one edge it stays low (a back button in a corner did not react with a bar of 500). The interface takes 150 as pressed and 80 as let go; untouched reads 0.
- `nocry-ui --sim SCRIPT` runs it on a PC without screen or touch chip (taps from a script, pictures to files, commands from `$PATH`): how the screens were checked before they ran on a board.

Not on the screen yet: uploading a firmware file (web pages only), boot loader and boot logo changes, files and logs.

### Remote access (Advanced > Remote access; `nocry-remote`; written 2026-11-27, not yet run on a board)

Three ways in over the network, all off unless the owner turns them on at the touch screen; what is on is kept with the settings and applied at the next start.

- **nocry over the network** (`nocry-netd`, port 5560, pairing code). It used to start by itself; since 2026-11-27 it does not. The web pages are not part of this: they are always there, behind the pairing code, which `nocry-remote` makes when `nocry-netd` is not running.
- **ADB over the network** (port 5555). The adbd in the image (Android 4.2) has no authentication and cannot be told at run time where to listen, so it is patched (`br-patches/android-tools`) to listen on 127.0.0.1:5554 beside USB, and `nocry-relay` opens port 5555 to the network only while this is on. The screen says plainly that whoever reaches the port is root.
- **SSH** (dropbear, port 22, user root). The password is `Creality2023` unless the owner sets another one on the screen; root has a password only while SSH is on (the serial console asks for none otherwise). Host keys are made at the first connection and live in memory, so they change with every start.

The plain recovery image (started over USB, no screen) has no Wi-Fi and none of this.

### USB flash drives (`nocry-usb`, `nocry-image`; written 2026-11-27; mounting and a backup ran on a Nebula Pad powered through its accelerometer socket, the rest not yet)

- **One controller, two uses.** The board has one USB controller. A computer on the USB-C socket supplies 5 V there, which the board sees on PD17; then the board is the USB device (adb, nocry over USB). Without it the board is the host: `nocry-usb` switches the role (`sw_switch_hsotg` of Ingenic's PHY driver) and the 5 V of the A sockets on (PC9, PC10). Pins and polarity are those of NebulaOS's device tree and of Creality's `usb_host_5v_enable.sh`. On a Nebula Pad powered through its USB-C socket the A sockets are dead (seen in NebulaOS too), so a flash drive needs the board powered through its ribbon cable, as it is on a printer. What `nocry-usb` decides is logged in `/run/nocry-usb.log`, which the log download includes.
- **Mounting.** The largest partition of the drive (or the drive itself) that mounts as FAT, exFAT, NTFS (Linux's ntfs3) or ext, at `/run/nocry-usb`; read-write if the file system allows it (an NTFS volume Windows did not shut down cleanly mounts read-only, and backups are refused with that reason).
- **Backup.** The whole eMMC or one partition, as a raw image with a `.sha256` beside it in `nocry-backups/` on the drive: the format of `nocry backup` on a computer. FAT32 holds no file of 4 GiB, so there an image is written in pieces (`.img.001`, `.img.002`, ...); the checksum is that of the pieces in a row (`cat` joins them). The file is read back from the drive and compared with the checksum before it counts. Names carry the model and the date (if the board is on a network that tells it) or a number, never the serial number.
- **Restore.** Listed are images up to three folders deep whose size is that of the eMMC, or of the partition their name ends in (`...-rootfs2.img`). The image is checked against its checksum first; then only megabytes that differ are written, and everything is read back. A whole-storage image whose identity record differs from the board's is refused (that is a job for nocry on a computer), and so is restoring `sn_mac` alone. A good copy of the rescue image on the board is left as it is.
- **Install.** Creality update files (`.img`) and factory packages (`.ingenic`) on the drive are listed and go through the same check, summary and install as a download; the file is read from the drive where it is.
- **Format.** Whenever a drive is plugged in, mounted or not (no file system, or one that is not known): after a warning that names the drive and its size, a new partition table with one partition, as exFAT or FAT32, labelled NOCRY. Only ever a USB disk.
- **Eject.** Writes everything out and unmounts. After a backup, a restore or an install from the drive the interface asks: Eject, Not now, or Don't ask again (kept). The drive is not mounted again until it has been pulled.

## Order of work

1. **Hardware bring-up from RAM:** a rescue kernel that shows a picture, reads touches and joins Wi-Fi on a Nebula Pad, loaded over USB like today's recovery. No storage or loader changes. This proves the base.
2. **Interface skeleton** with network setup and status; `nocry-fbd` over TCP with pairing; mDNS; nocry finds and drives it.
3. **Living on the board:** nocry installs the file and the pointer; nocryboot learns to load it; the boot logo program learns the touch trigger.
4. Slot and reset functions on the touchscreen; automatic start when nothing else starts.
5. K1 family (after its screens are verified with the boot logo program).
6. Firmware install on the printer.

## Bring-up log

2026-11-27, Nebula Pad, rescue kernel loaded from RAM over USB (`nocry recovery --image nocry-rescue.uImage`):

- **Screen: works.** Colour bars written to `/dev/fb0` (480x272, 32 bit) show correctly. The kernel's text console on the framebuffer hangs the driver's probe, so it is off. The frame buffer holds garbage until something draws: clear it first.
- **Backlight: works** (PWM0, enable PC22).
- **Touch controller: answers** at I2C address 0x48 on bus 4 (commands 0xC0/0xD0/0xE0 return X, Y, pressure). A bus scan shows nothing because the chip does not acknowledge empty probes.
- **Wi-Fi chip: found** ("new high speed SDIO card", BCM43430/1) once PA1 is driven low (it is active low) and the 32 kHz clock pin PE23 is switched to its function. It asks for `brcm/brcmfmac43430-sdio.bin` (+ `.AP6212` board file), which the initramfs does not carry yet.

Same day, with the system image that carries the firmware and tools:

- **Wi-Fi firmware loads** (`brcmfmac43430-sdio.bin`, version 7.45.98.118; it uses the `AP6212` board file), `wlan0` appears, and a scan lists the networks in range.
- **Access-point mode is supported and starts:** `wpa_supplicant` with `mode=2` brings up a WPA2 network on channel 6, `dnsmasq` hands out addresses. A phone joined it, got an address, and answered pings from the board.
- The interface comes up with the chip's own MAC address; rescue should set the one from `sn_mac`, as the firmware does.
- A press on the screen reads as pressure 1400 to 1750 with a stable position; no touch reads as pressure 0.

Build: `RESCUE=1 tools/recovery-build/build.sh` gives `nocry-rescue.uImage` (kernel + initramfs). With the Wi-Fi firmware, wpa_supplicant, iw, dnsmasq and mdnsd added it is 19.6 MiB, before any interface program and with all of linux-firmware's Broadcom files still in (only two are needed).

## Control over the LAN: what exists (2026-11-27)

- `tools/recovery-build/netd/nocry-netd.c` on the board: TCP port 5560, SPAKE2 pairing (RFC 9382, P-256) with a six-digit code, then ChaCha20-Poly1305 records, relayed to a `nocry-fbd --stdio` child. One client at a time; three wrong codes make a new code.
- `host/core/src/net.rs` on the PC: the same exchange; `Fastboot` runs over it as it does over USB. `nocry --net <address> --code <code> <any command>`.
- Both ends reproduce the RFC's test vector (`cargo test net::`, `nocry-netd --selftest`).
- Tested on a PC, service and client against a disk image: slots, loader status, a 48 MiB read and a 5 MiB write byte-identical, a verified boot logo change, wrong codes refused.
- Until the touch interface exists, `S45nocry-rescue` starts the board's own Wi-Fi network (`nocry`, WPA2, password `nocry111`, board at 192.168.77.1) and `nocry-banner` shows network, address and pairing code on the screen. The Wi-Fi password only gets a computer onto the network; driving the board needs the code.
- The image is 14.6 MiB with only this chip's Wi-Fi firmware.

- First run on a board (Nebula Pad, the TCP connection tunnelled over USB with `adb forward`): slots and loader read correctly, a 32 MiB read through the link is identical to the same read over USB (10.9 MB/s against 35 MB/s), a wrong code is refused.

- **Discovery** (`host/core/src/discover.rs`, `nocry discover`): nocry asks for `_nocry._tcp` once on every network interface, from an ordinary port, and takes the address the answer came from. Asked from an ordinary port, the board answers the sender directly, so nocry needs no mDNS service on the computer. The question must not carry the "unicast answer" bit: mdnsd 0.12 on the board ignores questions that do. The board's access point address (192.168.77.1) is also tried directly when the computer is on that network, for networks that do not pass mDNS. Tested against the board's own mdnsd (question sent on the board, its answer is a unit test) and against Avahi on a PC.
- **In the window:** "Connect over the network…" beside the state line (shown while no USB board is connected) lists the boards found, takes an address typed in, and asks for the pairing code. After pairing, every action in the window works on that board; the state line shows the address and a Disconnect button, and reports when the board stops answering. The last address is remembered, the code is not. Tested against a Nebula Pad through the USB tunnel: pairing, reading the board, a wrong code refused.

- **First run over Wi-Fi (from a Mac):** pairing worked, but reading the board's details did not finish: finding the firmware version read each slot's whole system partition, and the board was sending at 5.5 Mbit/s. The version lookup now fetches only the blocks it needs (about 0.4 MB of a 200 MB system, `squashfs::Paged`). The slow sending rate is unexplained.

- **Copying a slot** went through the computer (read slot A, send it back for slot B), which over Wi-Fi is the whole firmware twice through a slow link. `nocry-fbd` 0.4 has `nocry-copy`: the board copies, nocry asks for 16 MiB at a time, shows progress and compares the board's checksums of both sides. Tested on a PC against a disk image through `nocry-netd`: 512 MiB of slot in 0.6 s, both slots byte-identical, a repeat writes nothing. With an older service nocry copies the old way.

- **Erasing** sent the zeros from the computer. `nocry-zero` makes the board write them; it is used at the one place every write goes through, so partition erase, erase all, factory reset and the blank parts of an installation all benefit. Tested on a PC against a disk image: two partitions filled with random data, erased through `nocry-netd`, both read back as zeros.

### Getting onto a network

All of it is in `/usr/sbin/nocry-wifi` on the board (`overlay-rescue`). The board has one radio: it is either a client of a network or its own access point.

1. **The installed firmware's saved network.** At start rescue mounts `userdata` read-only and looks for a wpa_supplicant configuration with a network in it: `nebulaos/network/wpa_supplicant.conf` (NebulaOS), `nebulaos/wpa_supplicant.conf` (older NebulaOS), `wpa_supplicant.conf` (Creality's firmware). Its `network={}` blocks are used as they are, in memory only. The Wi-Fi address is set to the one in the identity record first, as Creality's firmware does, so a router sees the same device.
2. **The board's own network** when there is nothing saved or it cannot be joined: `nocry`, WPA2, password `nocry111` (short because it is typed on a phone; WPA2 takes nothing under 8 characters), board at 192.168.77.1. It is the fallback for everything, so a wrong password cannot lock anyone out.
3. **Choosing a network** from the own network:
   - **The set-up page** (`webd/nocry-webd.c`, http://192.168.77.1): for a phone or laptop without nocry. The access point resolves every name to the board, so most devices show the page by themselves after joining. It lists the networks in range and asks for the password and the pairing code on the screen. The code is asked because the access point's password is public; a wrong one costs two seconds. Plain HTML, no JavaScript, one request per connection, and the Wi-Fi tool is started with its arguments as they are (no shell).
   - **nocry:** `nocry wifi` (status and networks), `nocry wifi --join NAME --password P`, `nocry wifi --own`; in the window, "Board's Wi-Fi…" beside the state line. Service commands `nocry-wifi:status|scan|saved|ap|join:<name hex>[:<password hex>]`, offered only by the rescue system (`wifi` in `nocry-ext`).

A join answers at once and happens two seconds later, because over Wi-Fi it cuts the connection the request came through. If the network is not joined and an address obtained within about 45 seconds, the board reopens its own network and says why (`source=failed:<name>`). After a join requested over the network, nocry looks for the board on the networks the computer is on (mDNS, and the own network's fixed address) for three minutes and pairs again with the same code, which stays valid while rescue runs.

Limits: 2.4 GHz only; WPA2 passphrases (8 to 63 characters, no double quote) or open networks; the list of networks is the one taken at start, because this chip does not scan while it is an access point (a name can be typed). A chosen network is not remembered across a restart: rescue runs from memory and writes nothing.

### The web pages

`webd/nocry-webd.c` serves pages at the board's address on whatever network it is on, for someone with a phone or laptop and no nocry on it. On the board's own network every name resolves to the board, so most devices show them by themselves.

1. **Pairing code, once.** The first page asks only for the code on the board's screen. The browser then holds a session (a random token in a `SameSite=Strict`, `HttpOnly` cookie) until the board restarts. A wrong code costs two seconds; five in a row lock sign-in for a minute.
2. **Wi-Fi first.** While the board is on its own network, "Put the board on your Wi-Fi" is the first and highlighted card.
3. **Firmware:** what each slot holds (read from the slot's own system: Creality's `etc/ota_info` or NebulaOS's `os-release`), which one starts, a note when the nocryboot loader recorded a failed start; "Start slot X" and "Copy to slot Y".
4. **Reset:** "Remove system modifications" (blanks the settings overlay `rootfs_data` only; files and printer configuration stay) and "Factory reset" (that and `userdata`).

Every change asks first on a page of its own and says what it costs. Copy and the resets run as a job on the board with a progress bar (the page reloads itself every three seconds, no JavaScript) and a Cancel that says what stopping half-way leaves before it stops.

How it is built: the pages do no work themselves. `/usr/sbin/nocry-rescue-do` (shell) does the checks and runs jobs, and everything that touches the storage goes through the service's own commands with `nocry-fbd --exec <command>` (one command, answer as text, critical regions stay locked): `set_active`, `nocry-pcopy` (partition to partition, for slots), `nocry-zero`. So comparing, verified writing and the rules about what may be written exist once. Tools are started with their arguments as they are, never through a shell; actions are POST only.

Plain HTTP: on a network shared with others the code and the session can be overheard. A browser will not take an encrypted page from a board without a warning on every visit; nocry on a computer has its own encrypted link.

5. **Repair** (`/repair`): remove system modifications, check and repair a file system (`e2fsck -fy` on the data or the settings partition, as a job; the report is kept for the log download), factory reset.
6. **Files** (`/files`): the data partition (`userdata`, the firmware's `/usr/data`): browse, download, delete, upload a replacement of up to 8 MB (a corrected `printer.cfg`). It is mounted only while a request needs it, read-only unless something is changed, and paths that climb out of it are refused.
7. **Board** (`/board`): the storage chip's own wear estimate and spare-block state, how full the data partition is (with a warning when it is nearly full, a common reason for a printer that no longer starts); downloads: logs (the printer's, plus the rescue system's kernel log, board status and the last check report) as one `tar.gz`, the printer configuration, the identity record; "give the printer this Wi-Fi network", which writes the network the board is on into the installed firmware's own settings file; restart into the firmware.

8. **Install firmware** (`/install`): from the list for this board (the board downloads it; it has to be on a network with internet) or a file uploaded from the browser (up to 700 MB, streamed straight to the data partition). Two steps, as in the window: first the firmware is fetched, verified and checked against this board and the page says what would be done; only "Install now" writes. Either the firmware that starts is replaced with settings and files kept, or it goes into the other slot. A clean start is an install followed by a factory reset.
9. **Boot loader and boot logo** (`/boot`): what the board has, install nocryboot or put the factory loader back, nocry's logo or the firmware's.
10. **Full backup** (on `/board`): the whole storage as one gzip-compressed download, to restore with nocry on a computer.

Not on the pages yet: changing the printer type of a board (a conversion needs a clean install in one step), the kernel log of the last normal start, a restart into USB boot mode or into the other slot once.

### nocry itself on the board

Steps 8 and 9 are not a second implementation. The rescue system carries the `nocry` command-line program, built for the board's processor, and the pages run it: the same install logic, the same checks (which firmware fits which board, the identity record, NebulaOS's rules, verified writing) as on a computer.

- **Building it:** the processor is not a ready-made Rust target. `post-build.sh` builds `nocry` in the build container with a pinned nightly compiler, the standard library from source (`-Zbuild-std`), the target `mipsel-unknown-linux-gnu` with the feature `+nan2008` to match the system's C library, and Buildroot's gcc as the linker. It needs only the C library at run time and is 4.9 MB. The pin matters: `nan2008` given as a target feature is something newer compilers warn they will refuse; a target description file is the lasting answer.
- **Memory:** an install on a computer holds the firmware in memory several times over (300 to 435 MB at its peak). The board has about 230 MB in all. With `NOCRY_STAGE_DIR` set, parts are unpacked into files there and mapped (`blob.rs`), plans share them instead of copying, writes are merged up to 4 MiB instead of 64, and the 7z decoder runs on one thread (its parallel mode alone held several hundred MB). Measured on a PC under a hard limit: both an `.ingenic` package and a NebulaOS `.img` install in 40 MB, and an independent run confirms the result.
- **Where it works:** `NOCRY_BOARD_DISK` names the board's own storage; every command then works on it directly. The staging folder and downloads are on the data partition (`.nocry-rescue`, removed afterwards), which is mounted for as long as a firmware file is held. Because of that the on-board install never erases the data partition itself. A blank data partition gets a file system for the duration and is made blank again afterwards, so the firmware still finds it blank and sets it up its own way.
- **Downloads** go over https, which checks the server's certificate against the date, and the board has no clock that keeps time. The date is set once from the `Date` line of a plain-http answer.
- **Stopping:** `nocry` now treats a polite kill like Cancel in the window: it finishes the write under way, then stops. A second one ends it at once.
- New in the command line for this, usable anywhere: `nocry install <firmware> [--clean | --other-slot] [--yes]` (the window's install, with `--report` for other programs), `nocry catalog --tsv`.

### The same actions from a computer

`nocry-rescue-do` is in the plain recovery image too, and the service passes a fixed list of its verbs on (`nocry-do:<verb>[:<argument, hex>]`: status, health, job, cancel, check, ls, get, put, rm, bundle, wifi-save, restart; `do` in `nocry-ext`). nocry on a computer uses that over USB or the network: `nocry board health | check data|settings | ls [DIR] | get FILE -o OUT | put LOCAL FILE | rm FILE | logs -o OUT | config -o OUT | restart`. Answers of up to 32 MiB travel this way; larger files need the web pages or a backup. The window's dialogs for these are still to be written.

Tested on a PC: `nocry` for the board under an emulator (qemu) installing firmware onto a disk image, checked by the desktop build; the memory limits above; every install and boot page against stand-in tools; a 300 MB upload arriving byte-identical with 2 MB of server memory, and a cut-off upload leaving nothing behind. **Not yet run on a board.**

Tested (second round, 2026-11-27): browsing, downloads, an upload of text and of 2 MB of binary data, deleting, an over-size upload, an upload without a session, paths that climb out, all on a PC against a stand-in folder; on the Nebula Pad the read-only parts (storage report, file listing, log and identity downloads). Not yet run on a board: upload, delete, the file system check, saving the Wi-Fi network, restart.

Tested: every page and action on a PC with stand-in tools (sign-in, forged and missing sessions, odd characters, confirmations, a job and its Cancel); on the Nebula Pad over the home network, sign-in and a copy of slot A to slot B through the page (512 MiB in about 20 s), checked afterwards with nocry.

**On the Nebula Pad (2026-11-27):**

- No saved network on the test board (its data partition was blank), so it opened its own network; the set-up page came up on a phone by itself.
- Joining failed at first with "association rejected, status 16" and then a deauthentication ("previous authentication not valid"). With `brcmfmac.feature_disable=0x82000` on the kernel command line the same network was joined in five seconds. That value turns off the driver's use of the chip firmware's own WPA handshake (FWSUP) and SAE, so wpa_supplicant does the handshake. It is now in the rescue device tree. One caveat: the failed attempts were made with a 9-byte password and the successful one with an 8-byte one, so a mistyped password in the failed attempts cannot be ruled out; the setting is the documented cure for exactly these symptoms on this chip family and stays either way.
- With this driver a wrong password and a radio failure look the same from outside (the firmware reports both as status 16), which is why the script only says "turned away".
- As a client the board was found by mDNS from a PC on the same network, and a 32 MiB read through the encrypted link ran at 1.5 to 2.8 MB/s, against 0.1 to 0.3 MB/s when the board was the access point. The chip's calibration file made no clear difference: the generic AP6212 one from linux-firmware gave 2.0 MB/s, Creality's `nvram_azw372.txt` (which NebulaOS also uses) 1.5 to 2.8 MB/s, with the board 30 cm from the router (signal -12 dBm, probably too strong to be a fair test). The generic one stays.

Tested on a PC: the set-up page server (escaping, wrong code, odd characters in names and passwords), the service commands and nocry's side against a stand-in Wi-Fi tool, the script's text handling with the board's own shell tools. Not yet run on a board.

Missing: a run over the board's Wi-Fi instead of the tunnel (the development PC has no Wi-Fi adapter), and the model and serial number tail in the announcement so two boards can be told apart in the list.

## Open points

- NS2009: kernel driver or user-space reader; calibration.
- Size: measure after step 1; the 20 to 40 MiB estimate decides how long the loader's checksum takes.
- Bluetooth is not used.
