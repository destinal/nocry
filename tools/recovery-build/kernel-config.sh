#!/bin/sh
# Applied on top of k1_defconfig (k1-debian/linux master-k1) for the nocry recovery kernel.
# usage: KSRC=<kernel src> KOUT=<build dir> kernel-config.sh <initramfs.cpio or ""> [rescue]
#   rescue: the nocry-rescue kernel: the same plus screen, backlight, touch bus and Wi-Fi
set -e
C="${KSRC:-.}/scripts/config --file ${KOUT:-.}/.config"
$C -d DT_CREALITY_K1 -e DT_NOCRY_NEBULA
$C -e MIPS_CMDLINE_DTB_EXTEND -d MIPS_CMDLINE_FROM_BOOTLOADER -d MIPS_CMDLINE_FROM_DTB -d CMDLINE_BOOL
$C -e BLK_DEV_INITRD --set-str INITRAMFS_SOURCE "$1" -e INITRAMFS_COMPRESSION_GZIP \
   --set-val INITRAMFS_ROOT_UID 0 --set-val INITRAMFS_ROOT_GID 0
# USB device mode: ADB (FunctionFS) + ACM console + optional mass storage, all via configfs
$C -d USB_DWC2_HOST -e USB_DWC2_PERIPHERAL -d USB_DWC2_DUAL_ROLE
$C -e USB_GADGET -e USB_LIBCOMPOSITE -e USB_CONFIGFS -e USB_CONFIGFS_F_FS -e USB_CONFIGFS_ACM \
   -e USB_CONFIGFS_SERIAL -e USB_CONFIGFS_MASS_STORAGE -e USB_F_FS -e USB_F_ACM -e USB_F_MASS_STORAGE \
   -e USB_U_SERIAL
# no legacy gadgets: CONFIG_USB_FUNCTIONFS is the old built-in g_ffs gadget, which claims the
# FunctionFS device at boot (configfs `mkdir functions/ffs.adb` then fails with EBUSY)
$C -d USB_FUNCTIONFS -d USB_FUNCTIONFS_GENERIC -d USB_FUNCTIONFS_ETH -d USB_FUNCTIONFS_RNDIS
# filesystems useful for repair
# every compression a firmware's system uses: Creality gzip or lzo, NebulaOS zstd (the rescue pages read the version from it)
$C -e SQUASHFS -e SQUASHFS_XZ -e SQUASHFS_ZLIB -e SQUASHFS_LZO -e SQUASHFS_ZSTD -e EXT4_FS -e EXT2_FS -e VFAT_FS -e DEVTMPFS -e DEVTMPFS_MOUNT -e CONFIGFS_FS -e TMPFS
# no eFuse access, no display stack
$C -d INGENIC_EFUSE_X2000 -d INGENIC_EFUSE_WRITABLE
$C -d FB_INGENIC -d STAGE_CREALITY_LCD -d LOGO -d TRUE_COLOR_LOGO
# never reset on its own: a reset boots the stock firmware, which wipes the nocrymem log in DDR
$C -d INGENIC_WDT -d WATCHDOG_HANDLE_BOOT_ENABLED --set-val PANIC_TIMEOUT 0
# USB mass storage (nocry.ums=ro|rw): deeper pipeline than the default 2 x 16 KiB buffers
$C --set-val USB_GADGET_STORAGE_NUM_BUFFERS 32

if [ "${2:-}" = rescue ]; then
  $C -d DT_NOCRY_NEBULA -e DT_NOCRY_NEBULA_RESCUE
  # screen: Ingenic display controller + the Nebula panel. No kernel text console on it: with
  # FRAMEBUFFER_CONSOLE this driver hangs in probe (seen on the Nebula; Creality's config has it off too)
  $C -e FB_INGENIC -e FB_INGENIC_DISPLAYS_STAGE -e STAGE_NOCRY_NEBULA -d STAGE_CREALITY_LCD -d FRAMEBUFFER_CONSOLE
  $C -e BACKLIGHT_CLASS_DEVICE -e BACKLIGHT_PWM -e PWM -e PWM_INGENIC_V2
  # touch: the NS2009 is read from user space; a USB keyboard or mouse can stand in
  $C -e I2C_CHARDEV -e INPUT_EVDEV
  # Wi-Fi: BCM43438 over SDIO, built in (the firmware comes from the initramfs)
  $C -e CFG80211 -e BRCMFMAC -e BRCMFMAC_SDIO -e PWRSEQ_SIMPLE -e REGULATOR_FIXED_VOLTAGE -d CFG80211_REQUIRE_SIGNED_REGDB
  $C -e PACKET -e INET -e IPV6
  # one controller, two uses: device for a computer on the USB-C socket, host for a flash
  # drive in an A socket (nocry-usb decides); the file systems such drives carry
  $C -d USB_DWC2_PERIPHERAL -d USB_DWC2_HOST -e USB_DWC2_DUAL_ROLE -e USB_STORAGE -e SCSI -e BLK_DEV_SD
  $C -e EXFAT_FS --set-str EXFAT_DEFAULT_IOCHARSET utf8 -e NTFS3_FS -e FAT_DEFAULT_UTF8 -e NLS_UTF8 -e MSDOS_PARTITION -e DEBUG_FS
fi
