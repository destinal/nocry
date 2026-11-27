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
| Starting it | finger on the screen at power-on, then a 5-second confirmation drawn by the boot picture program; automatically when no slot can be started; on request (a one-shot note in `ota`) |
| USB keyboard as trigger | not possible: the loader has no USB stack and about 1.6 KB of room |
| First version | **decided:** Nebula Pad first. Network setup, slot choice, factory reset, start normally, control from nocry over the LAN. |
| Firmware install from the touchscreen | **decided:** second step (needs nocry's install engine built for the board) |
| LAN access | proposed: pairing code shown on the touchscreen, typed into nocry on the PC; discovery by mDNS |
| Interface | LVGL, starting from GuppyScreen / NebulaOS-nebulascreen (GPL-3.0); rescue's interface is its own GPL-3.0 program |

## Why a file and a pointer

Rescue with a touch interface, Wi-Fi and its firmware is estimated at 20 to 40 MiB. The first partitions have no such room: `ota` and `sn_mac` are 1 MiB each, the `rtos` partitions 4 MiB each (nocry's boot picture program is 32 KB, so they are nearly empty, but that is still under 8 MiB together), and each `kernel` partition has about 4 MiB to spare.

`userdata` has gigabytes, but it is an ext4 file system and nocryboot's first stage cannot read one. It does not have to:

1. nocry (from the recovery system) writes the rescue image as a single unfragmented file, `nocry-rescue.img`, and sets it immutable, so the firmware does not move or delete it.
2. nocry asks the file system for the file's physical place (FIEMAP), refuses anything but one extent, and writes into sector 2 of `ota`:
   `nocryboot:rescue=<start sector>:<sectors>:<crc32>`
3. nocryboot, when rescue is wanted, loads those sectors like a kernel image (it is a uImage), checks the checksum, and starts it. A failed check means "no rescue": it goes on to the normal start or to USB boot mode.

The loader never writes there and never trusts the pointer without the checksum, so a stale pointer after a factory reset is harmless.

## Starting it

The first stage has almost no room and no input devices. The boot picture program (`rtos/`) runs before Linux, owns the screen, and has room:

- It reads the touch controller. A finger held down at power-on brings up "Start nocry-rescue? Keep holding / tap to confirm" for 5 seconds. Without a finger, nothing changes and no time is added.
- It reports the result to the first stage in the word it already uses for the panel id (a flag bit beside it).
- The first stage then loads rescue instead of the slot's kernel.

Also:

- **No startable slot:** today nocryboot restarts into USB boot mode. With rescue present it starts rescue first.
- **On request:** `nocryboot:rescue-once` in the `ota` notes, set by nocry or by a command on the running printer; the loader clears it when it acts on it.

Rescue is started with the display already switched on by the boot picture program, like the firmware's Linux is.

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
- Everything the window does over USB then works over the LAN: backup, restore, install, slots, factory reset, identity, loader, boot picture.

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
3. **Living on the board:** nocry installs the file and the pointer; nocryboot learns to load it; the boot picture program learns the touch trigger.
4. Slot and reset functions on the touchscreen; automatic start when nothing else starts.
5. K1 family (after its screens are verified with the boot picture program).
6. Firmware install on the printer.

## Bring-up log

2026-11-27, Nebula Pad, rescue kernel loaded from RAM over USB (`nocry recovery --image nocry-rescue.uImage`):

- **Screen: works.** Colour bars written to `/dev/fb0` (480x272, 32 bit) show correctly. The kernel's text console on the framebuffer hangs the driver's probe, so it is off. The frame buffer holds garbage until something draws: clear it first.
- **Backlight: works** (PWM0, enable PC22).
- **Touch controller: answers** at I2C address 0x48 on bus 4 (commands 0xC0/0xD0/0xE0 return X, Y, pressure). A bus scan shows nothing because the chip does not acknowledge empty probes.
- **Wi-Fi chip: found** ("new high speed SDIO card", BCM43430/1) once PA1 is driven low (it is active low) and the 32 kHz clock pin PE23 is switched to its function. It asks for `brcm/brcmfmac43430-sdio.bin` (+ `.AP6212` board file), which the initramfs does not carry yet.

Build: `RESCUE=1 tools/recovery-build/build.sh` gives `nocry-rescue.uImage` (kernel + initramfs). With the Wi-Fi firmware, wpa_supplicant, iw, dnsmasq and mdnsd added it is 19.6 MiB, before any interface program and with all of linux-firmware's Broadcom files still in (only two are needed).

## Open points

- Whether brcmfmac's access-point mode works with this chip's firmware in practice.
- NS2009: kernel driver or user-space reader; calibration.
- Size: measure after step 1; the 20 to 40 MiB estimate decides how long the loader's checksum takes.
- Bluetooth is not used.
