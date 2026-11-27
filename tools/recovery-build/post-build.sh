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
  "$HOST_DIR/bin/mipsel-linux-gcc" -O2 -Wall -o "$T/usr/sbin/nocry-webd" "$HERE/webd/nocry-webd.c"
  "$HOST_DIR/bin/mipsel-linux-strip" "$T/usr/sbin/nocry-netd" "$T/usr/sbin/nocry-banner" "$T/usr/sbin/nocry-webd"
  chmod 755 "$T/usr/sbin/nocry-wifi" "$T/etc/init.d/S45nocry-rescue"
  # started by S45nocry-rescue with our own settings, not by the packages' scripts
  rm -f "$T/etc/init.d/S80dnsmasq" "$T/etc/init.d/S50mdnsd" "$T/etc/mdns.d/ssh.service"
  # of the Broadcom firmware collection only this board's chip is needed
  # (in linux-firmware the brcm/ names of this chip are links into cypress/)
  for d in brcm cypress; do
    find "$T/lib/firmware/$d" \( -type f -o -type l \) ! -name '*43430-sdio.bin' ! -name '*43430-sdio.clm_blob' \
      ! -name 'brcmfmac43430-sdio.AP6212.txt' -delete 2>/dev/null || true
  done
  [ -e "$T/lib/firmware/brcm/brcmfmac43430-sdio.bin" ] || { echo "post-build: the Wi-Fi firmware is missing" >&2; exit 1; }
fi
