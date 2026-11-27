#!/bin/sh
# nocryboot: report that this start worked.
#
# When a firmware slot is "on trial", the boot loader keeps a count of starts in
# the second sector of the ota partition (text: nocryboot:try=B:left=2). Erasing
# that sector tells it the slot works; otherwise it returns to the other slot
# when the count runs out. Installed by nocry; see docs/nocryboot.md.
#
# Runs last at start-up, then waits a little so that a system which goes down
# right after start-up does not count as working.

confirm() {
    sleep 30
    for p in /sys/block/mmcblk0/mmcblk0p*; do
        grep -q '^PARTNAME=ota$' "$p/uevent" 2>/dev/null || continue
        dev=/dev/${p##*/}
        if [ "$(dd if="$dev" bs=1 skip=512 count=14 2>/dev/null)" = "nocryboot:try=" ]; then
            dd if=/dev/zero of="$dev" bs=512 seek=1 count=1 conv=fsync 2>/dev/null
        fi
    done
}

case "$1" in
  start) confirm & ;;
esac
exit 0
