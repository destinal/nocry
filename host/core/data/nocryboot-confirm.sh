#!/bin/sh
# nocryboot: report that this start worked.
#
# When a firmware slot is "on trial", the boot loader keeps a count of starts in
# the second sector of the ota partition (text: nocryboot:try=B:left=2). Erasing
# that sector tells it the slot works; otherwise it returns to the other slot
# when the count runs out. Installed by nocry; see docs/nocryboot.md.
#
# Started before the printer's own programs (in case one of those start-up
# scripts does not return), it then waits in the background so that a system
# which goes down right after start-up does not count as working.

LOG=/usr/data/nocryboot-confirm.log

# The ota partition: by name where the kernel tells it, else the first partition
# (where it is on every known board). Nothing is erased unless the trial text is there.
ota_dev() {
    for p in /sys/block/mmcblk0/mmcblk0p*; do
        if grep -q '^PARTNAME=ota$' "$p/uevent" 2>/dev/null; then
            echo "/dev/${p##*/}"
            return
        fi
    done
    echo /dev/mmcblk0p1
}

confirm() {
    trap '' HUP          # keep going when the shell that started us ends
    sleep 45
    dev=$(ota_dev)
    note=$(dd if="$dev" bs=1 skip=512 count=22 2>/dev/null)
    case "$note" in
      nocryboot:try=*)
        dd if=/dev/zero of="$dev" bs=512 seek=1 count=1 2>/dev/null
        sync
        echo "$(date) $dev: '$note' -> reported success" > "$LOG" 2>/dev/null
        ;;
    esac
}

case "$1" in
  start) confirm </dev/null >/dev/null 2>&1 & ;;
esac
exit 0
