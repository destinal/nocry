#!/usr/bin/env bash
# Runs the factory SPL (taken from a backup image of a board) and nocryboot's SPL in the
# emulator and prints where their hardware accesses and kernel arguments differ.
#
# usage: compare.sh <nebula|k1> <backup image of that board> [extra splemu.py options]
#   e.g. compare.sh nebula backups/nebula-full.img --lcm 3
# Needs tools/nocryboot-build/out (run build.sh first), python3 with unicorn, binutils (nm).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
FAM=$1; IMG=$2; shift 2
B="$HERE/../nocryboot-build/out"; O="$HERE/out"; mkdir -p "$O"
# the factory loader: 24 KiB at byte 0x4400 of the eMMC
dd if="$IMG" of="$O/stock-$FAM.bin" bs=512 skip=34 count=48 status=none
CMD=$(nm "$B/nocryboot_$FAM/u-boot-spl" | awk '$3=="mmc_cmd"{print $1}')
python3 "$HERE/splemu.py" "$B/nocryboot-$FAM.bin" "$IMG" --elf "$B/nocryboot_$FAM/u-boot-spl" --cmd "$CMD" \
        -o "$O/trace-nocryboot-$FAM.txt" "$@" > /dev/null
# Routine addresses in Creality's SPL (builds of Oct/Nov 2023; both families share them):
# read blocks, send command, and the two delay routines.
python3 "$HERE/splemu.py" "$O/stock-$FAM.bin" "$IMG" --block-read b2403724 --cmd b24032d0 \
        --delay b2404930 --delay b24048c8 -o "$O/trace-stock-$FAM.txt" "$@" > /dev/null
for f in stock nocryboot; do
  grep -v '^# uart\|^$\|first words\|emulator error' "$O/trace-$f-$FAM.txt" |
    sed 's/ *\[0x[0-9a-f]*\]$//; s/entry 0x[0-9a-f]*/entry X/' > "$O/norm-$f-$FAM.txt"
done
echo "factory: $(grep -c '^W' "$O/norm-stock-$FAM.txt") register writes; nocryboot: $(grep -c '^W' "$O/norm-nocryboot-$FAM.txt")"
diff "$O/norm-stock-$FAM.txt" "$O/norm-nocryboot-$FAM.txt" && echo "no differences"
