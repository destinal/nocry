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
# the board's own repair actions, in the recovery and in the rescue system alike: nocry on a
# computer reaches them through the service (nocry-do), the rescue pages run them directly
chmod 755 "$T/usr/sbin/nocry-rescue-do"

# nocry-rescue only (its system has the Wi-Fi tools): the network service and the status screen
if [ -e "$T/usr/sbin/wpa_supplicant" ]; then
  "$HOST_DIR/bin/mipsel-linux-gcc" -O2 -Wall -Wno-deprecated-declarations -o "$T/usr/sbin/nocry-netd" "$HERE/netd/nocry-netd.c" -lcrypto
  "$HOST_DIR/bin/mipsel-linux-gcc" -O2 -Wall -o "$T/usr/sbin/nocry-banner" "$HERE/screen/nocry-banner.c"
  "$HOST_DIR/bin/mipsel-linux-gcc" -O2 -Wall -o "$T/usr/sbin/nocry-webd" "$HERE/webd/nocry-webd.c"
  "$HOST_DIR/bin/mipsel-linux-strip" "$T/usr/sbin/nocry-netd" "$T/usr/sbin/nocry-banner" "$T/usr/sbin/nocry-webd"
  chmod 755 "$T/usr/sbin/nocry-wifi" "$T/etc/init.d/S45nocry-rescue"
  # nocry itself, for the board: the rescue pages install firmware, the boot loader and the boot
  # logo with the same program and checks as on a computer (docs/nocry-rescue.md)
  if [ -d /repo/host ] && command -v cargo >/dev/null 2>&1; then
    CARGO_HOME=/w/cargo-home CARGO_TARGET_DIR=/w/board-target \
    CARGO_TARGET_MIPSEL_UNKNOWN_LINUX_GNU_LINKER="$HOST_DIR/bin/mipsel-linux-gcc" \
    CC_mipsel_unknown_linux_gnu="$HOST_DIR/bin/mipsel-linux-gcc" AR_mipsel_unknown_linux_gnu="$HOST_DIR/bin/mipsel-linux-ar" \
    RUSTFLAGS="-C target-feature=+nan2008 -Zunstable-options -C linker-flavor=gnu-cc -C link-self-contained=no" \
      cargo build --release --locked -Zbuild-std=std,panic_abort --target mipsel-unknown-linux-gnu -p nocry --manifest-path /repo/host/Cargo.toml
    "$HOST_DIR/bin/mipsel-linux-strip" -o "$T/usr/sbin/nocry" /w/board-target/mipsel-unknown-linux-gnu/release/nocry
    mkdir -p "$T/usr/share/nocry"
    rm -rf "$T/usr/share/nocry/images"
    [ -d /images/nocryboot ] && cp -r /images "$T/usr/share/nocry/images"
  else
    echo "error: cannot build nocry for the board (no /repo/host or no cargo in the build container)" >&2; exit 1
  fi
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
