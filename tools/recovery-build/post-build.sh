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
