# nocry-rescue: design

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
| Where it lives | **decided:** one file in `userdata`, not a partition. No repartitioning. |
| How the loader finds it | a pointer (start sector, length, checksum) in a spare sector of `ota`; the loader reads raw sectors and checks them before starting anything |
| Losing it | **decided, accepted for now:** a factory reset erases `userdata` and with it the file. A reset done by nocry or by rescue puts it back; after a reset from Creality's menu it is gone until nocry reinstalls it. A dedicated partition would survive; possible later. |
| Starting it | finger on the screen at power-on, then a 5-second confirmation drawn by the boot logo program; automatically when no slot can be started; on request (a one-shot note in `ota`) |
| USB keyboard as trigger | not possible: the loader has no USB stack and about 1.6 KB of room |
| First version | **decided:** Nebula Pad first. Network setup, slot choice, factory reset, start normally, control from nocry over the LAN. |
| Firmware install from the touchscreen | **decided:** second step (needs nocry's install engine built for the board) |
| LAN access | proposed: pairing code shown on the touchscreen, typed into nocry on the PC; discovery by mDNS |
| Interface | LVGL, starting from GuppyScreen / NebulaOS-nebulascreen (GPL-3.0); rescue's interface is its own GPL-3.0 program |

## Why a file and a pointer

Rescue with a touch interface, Wi-Fi and its firmware is estimated at 20 to 40 MiB. The first partitions have no such room: `ota` and `sn_mac` are 1 MiB each, the `rtos` partitions 4 MiB each (nocry's boot logo program is 32 KB, so they are nearly empty, but that is still under 8 MiB together), and each `kernel` partition has about 4 MiB to spare.

`userdata` has gigabytes, but it is an ext4 file system and nocryboot's first stage cannot read one. It does not have to:

1. nocry (from the recovery system) writes the rescue image as a single unfragmented file, `nocry-rescue.img`, and sets it immutable, so the firmware does not move or delete it.
2. nocry asks the file system for the file's physical place (FIEMAP), refuses anything but one extent, and writes into sector 2 of `ota`:
   `nocryboot:rescue=<start sector>:<sectors>:<crc32>`
3. nocryboot, when rescue is wanted, loads those sectors like a kernel image (it is a uImage), checks the checksum, and starts it. A failed check means "no rescue": it goes on to the normal start or to USB boot mode.

The loader never writes there and never trusts the pointer without the checksum, so a stale pointer after a factory reset is harmless.

## Starting it

The first stage has almost no room and no input devices. The boot logo program (`rtos/`) runs before Linux, owns the screen, and has room:

- It reads the touch controller. A finger held down at power-on brings up "Start nocry-rescue? Keep holding / tap to confirm" for 5 seconds. Without a finger, nothing changes and no time is added.
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

Not on the pages yet: installing firmware (needs nocry's own install logic on the board, see the to-do list), full backups, the boot loader and boot logo, the kernel log of the last normal start, a restart into USB boot mode or into the other slot once.

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
