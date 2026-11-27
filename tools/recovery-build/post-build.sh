#!/bin/sh
# Buildroot post-build: root shell on the USB serial (ACM) console, no login.
# The id field stays empty: busybox init would otherwise open /dev/ttyGS0 itself,
# before the gadget (S30nocry-usb) has created it.
set -e
T="$1"
sed -i '/ttyGS0/d' "$T/etc/inittab"
sed -i '/# GENERIC_SERIAL/a ::respawn:/bin/sh -c "while [ ! -e /dev/ttyGS0 ]; do sleep 1; done; exec /sbin/getty -n -l /bin/sh -L 115200 ttyGS0 vt100"' "$T/etc/inittab"
# nocry-fbd: fastboot service (fbd/nocry-fbd.c), built with Buildroot's toolchain
HERE=$(cd "$(dirname "$0")" && pwd)
"$HOST_DIR/bin/mipsel-linux-gcc" -O2 -Wall -pthread -o "$T/usr/sbin/nocry-fbd" "$HERE/fbd/nocry-fbd.c" -lcrypto
"$HOST_DIR/bin/mipsel-linux-strip" "$T/usr/sbin/nocry-fbd"

# nocry-rescue only (its system has the Wi-Fi tools): the network service and the status screen
if [ -e "$T/usr/sbin/wpa_supplicant" ]; then
  "$HOST_DIR/bin/mipsel-linux-gcc" -O2 -Wall -Wno-deprecated-declarations -o "$T/usr/sbin/nocry-netd" "$HERE/netd/nocry-netd.c" -lcrypto
  "$HOST_DIR/bin/mipsel-linux-gcc" -O2 -Wall -o "$T/usr/sbin/nocry-banner" "$HERE/screen/nocry-banner.c"
  "$HOST_DIR/bin/mipsel-linux-strip" "$T/usr/sbin/nocry-netd" "$T/usr/sbin/nocry-banner"
  # started by S45nocry-rescue with our own settings, not by the packages' scripts
  rm -f "$T/etc/init.d/S80dnsmasq" "$T/etc/init.d/S50mdnsd" "$T/etc/mdns.d/ssh.service"
  # of the Broadcom firmware collection only this board's chip is needed
  find "$T/lib/firmware/brcm" -type f ! -name 'brcmfmac43430-sdio.bin' ! -name 'brcmfmac43430-sdio.AP6212.txt' ! -name 'brcmfmac43430-sdio.clm_blob' -delete 2>/dev/null || true
  find "$T/lib/firmware" -type l ! -exec test -e {} \; -delete 2>/dev/null || true
  rm -rf "$T/lib/firmware/cypress"
fi
