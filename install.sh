#!/bin/sh
# nocry: install the nocryboot loader and the nocry-rescue system on a printer, from the
# printer itself. SPDX-License-Identifier: GPL-2.0-or-later
#
# Run it in a root shell on the printer (ssh, or the serial console), with the printer online:
#
#     curl -fsSL https://raw.githubusercontent.com/destinal/nocry/main/install.sh | sh
#
# Afterwards: switch the printer off, put a finger on the screen, switch it on. The rescue
# system starts instead of the firmware. Without the finger everything starts as before.
#
# For Creality's Nebula Pad family (retail Nebula Pad, Ender-3 V3 KE, CR-10 SE, Ender-5 Max)
# running its normal firmware. Other boards are refused: the K1 family's screen cannot start
# the rescue system yet.
#
# What it does, and what it checks before it writes anything:
#   1. The board: the loader on it is Creality's or an earlier nocryboot, built for this board
#      family; every partition the new loader reads is where the loader will look for it; the
#      system images leave the last 50 MiB of their partitions free.
#   2. The newest release of github.com/destinal/nocry: the rescue image and the loader for
#      this family, each downloaded over https and kept only if its size and SHA-256 are what
#      GitHub lists for the release, and if it is what its name says (the loader: 24 KiB, for
#      this family, that version, not a test build; the rescue image: a kernel image by that
#      name).
#   3. It says what it is about to do and asks (unless -y).
# Then, in this order, so that at every moment the board has a loader that starts:
#   4. The rescue image, into the last 50 MiB of both firmware slots' partitions, read back.
#   5. Creality's loader, kept: a copy on the board (where `nocry loader --factory` finds it)
#      and one as a file on the data partition. Read back.
#   6. The new loader, 24 KiB at byte 0x4400. Read back; if it does not read back right, the
#      loader that was there is written again.
# Nothing else is touched: not the firmware, not the settings, not the identity record.
#
#   install.sh [-y] [--check]
#     -y        do not ask
#     --check   say what would be done, change nothing
#
# To undo: `nocry loader --factory --yes` and `nocry rescue --remove` from a computer.
set -u

DISK=${NOCRY_DISK:-/dev/mmcblk0}                     # elsewhere only in tests
SYS=${NOCRY_SYS:-/sys}
API=${NOCRY_API:-https://api.github.com/repos/destinal/nocry/releases?per_page=20}
DOWNLOADS=https://github.com/destinal/nocry/releases/download/
KEEP_DIR=${NOCRY_KEEP_DIR:-/usr/data}
TMP=${TMPDIR:-/tmp}/nocry-install.$$
ROOM_MIB=50
SPL_SECTOR=34            # byte 0x4400
SPL_SECTORS=48           # 24 KiB
MARK_SECTOR=1919         # byte 0xefe00: one sector of text saying a factory loader is kept after it
KEPT_SECTOR=1920         # byte 0xf0000

YES=; CHECK=
for a in "$@"; do
    case "$a" in -y|--yes) YES=1 ;; --check) CHECK=1 ;; *) echo "usage: install.sh [-y] [--check]" >&2; exit 2 ;; esac
done

say() { printf '%s\n' "$*"; }
die() { printf '\nnocry install: %s\n' "$*" >&2; [ -n "${CHANGED:-}" ] || printf 'Nothing was changed.\n' >&2; rm -rf "$TMP"; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "this system has no '$1'"; }
sha() { sha256sum | cut -c1-64; }
bytes() { wc -c < "$1" | tr -d ' '; }
# what is on the storage: sectors START COUNT
rd() { dd if="$DISK" bs=512 skip="$1" count="$2" 2>/dev/null; }
fresh() { sync; [ -w /proc/sys/vm/drop_caches ] && echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; return 0; }   # read back from the chip, not from memory

for t in sha256sum dd awk grep sed tr cut head wc cmp; do need "$t"; done
[ -n "${NOCRY_DISK:-}" ] || [ "$(id -u 2>/dev/null || echo 0)" = 0 ] || die "this has to run as root"
[ -e "$DISK" ] || die "no $DISK: this is not a printer's own system"
[ -n "${NOCRY_DISK:-}" ] || [ -w "$DISK" ] || die "$DISK cannot be written"
mkdir -p "$TMP" || die "cannot create $TMP"

# ---- 1. the board ------------------------------------------------------------------------
# What a 24 KiB loader is: prints "factory FAMILY", "nocryboot VERSION FAMILY", or "unknown".
inspect() {
    f=$1
    [ "$(bytes "$f")" = 24576 ] && [ "$(dd if="$f" bs=1 count=4 2>/dev/null)" = LPSM ] && [ "$(dd if="$f" bs=1 skip=256 count=4 2>/dev/null)" = INGE ] || { echo unknown; return; }
    fam=
    if grep -qa "console=ttyS4," "$f" && ! grep -qa "console=ttyS2," "$f"; then fam=nebula
    elif grep -qa "console=ttyS2," "$f" && ! grep -qa "console=ttyS4," "$f"; then fam=k1; fi
    nb=$(tr -c 'a-z0-9. -' '\n' < "$f" | sed -n 's/^.*nocryboot-loader \([0-9][0-9.]*\)\(-[a-z]*\)\{0,1\} \([a-z0-9]*\)$/\1\2 \3/p' | head -1)
    if [ -n "$nb" ]; then echo "nocryboot $nb"; return; fi
    # everything Creality's loader needs to start a slot, and the mark only theirs has
    if grep -qa "ota:kernel2" "$f" && grep -qa "sn_mac" "$f" && grep -qa "lcm_id=" "$f" && grep -qa "\[lcm-start" "$f" &&
       ! grep -qa "nocryboot" "$f" && ! grep -qa "restarting in USB boot mode" "$f" && [ -n "$fam" ]; then echo "factory $fam"; return; fi
    echo unknown
}
rd $SPL_SECTOR $SPL_SECTORS > "$TMP/loader.now"
set -- $(inspect "$TMP/loader.now"); NOW_KIND=${1:-unknown}
case "$NOW_KIND" in
  factory) NOW_VER=; FAMILY=$2; NOW_TEXT="Creality's loader" ;;
  nocryboot) NOW_VER=$2; FAMILY=$3; NOW_TEXT="nocryboot $2" ;;
  *) die "the boot loader on this board is neither Creality's nor nocryboot. This script does not replace loaders it does not know" ;;
esac
case "$NOW_VER" in *doortest*) die "the loader on this board is a test build" ;; esac
[ "$FAMILY" = nebula ] || die "this board is of the K1 family. Its screen cannot start the rescue system yet; use nocry on a computer for its loader"

# the partitions, from the kernel's own view of the table: NAME -> start and size in sectors
part() {    # part NAME start|size
    for u in "$SYS"/class/block/mmcblk0p*/uevent; do
        if grep -q "^PARTNAME=$1\$" "$u" 2>/dev/null; then cat "$(dirname "$u")/$2"; return 0; fi
    done
    return 1
}
# where the loader looks for them (it has its own table and does not read the one on the disk): MiB
for want in ota:1 sn_mac:2 rtos:3 rtos2:7 kernel:11 kernel2:19 rootfs:27 rootfs2:527; do
    n=${want%:*}; at=${want#*:}
    s=$(part "$n" start) || die "this board has no partition '$n'; its layout does not fit the loader"
    [ "$s" = $((at * 2048)) ] || die "partition '$n' starts at sector $s, but the loader looks for it at $((at * 2048)); this board's layout does not fit the loader"
done
# the identity record must not name another family's mainboard
case "$(rd $(part sn_mac start) 1 | tr '\000' '\n' | head -1 | cut -d';' -f4)" in
  CR4CU*) die "the identity record names a K1 mainboard, but the loader on the board is for the Nebula family. Not touching it" ;;
esac
# the room for the rescue image: the last 50 MiB of each root file system partition, which must
# lie behind the end of the system image (a squashfs says how long it is)
ROOM_AT=
for n in rootfs rootfs2; do
    s=$(part $n start); z=$(part $n size)
    [ $((z % 2048)) = 0 ] && [ "$z" -ge $((ROOM_MIB * 2 * 2048)) ] || die "partition $n has an unexpected size ($z sectors)"
    if [ "$(rd "$s" 1 | dd bs=1 count=4 2>/dev/null)" = hsqs ]; then
        used=$(rd "$s" 1 | dd bs=1 skip=40 count=8 2>/dev/null | od -An -tu4 | awk '{ printf "%.0f", $1 + $2 * 4294967296 }')
        [ "$used" -le $(( (z / 2048 - ROOM_MIB) * 1048576 )) ] || die "the system image in $n reaches into the last $ROOM_MIB MiB of its partition; there is no room for the rescue image"
    fi
    ROOM_AT="$ROOM_AT $(( (s + z) / 2048 - ROOM_MIB ))"       # MiB from the start of the storage
done
say "Board: Nebula family, $NOW_TEXT, partitions where the loader expects them."

# ---- 2. the newest release ------------------------------------------------------------------
fetch() {   # fetch URL FILE
    if command -v curl >/dev/null 2>&1; then curl -fsSL --retry 2 -o "$2" "$1"
    else wget -q -O "$2" "$1"; fi
}
case "$API" in
  https://*|http://*) fetch "$API" "$TMP/releases.json" || die "GitHub could not be reached (is the printer online? is its clock right?)" ;;
  *) cp "$API" "$TMP/releases.json" || die "no such file: $API" ;;
esac
# one line for each file of each release: TAG NAME SIZE SHA256 URL (GitHub's answer has one field per line)
awk -F'"' '
    /^    "tag_name":/ { tag = $4 }
    /^        "name":/ { name = $4 }
    /^        "size":/ { size = $3; gsub(/[^0-9]/, "", size) }
    /^        "digest":/ { dig = $4; sub(/^sha256:/, "", dig) }
    /^        "browser_download_url":/ { if (tag != "" && name != "" && length(dig) == 64) print tag, name, size, dig, $4; name = ""; dig = ""; size = "" }
' "$TMP/releases.json" > "$TMP/files"
vkey() {    # a version as something that sorts: 0.2.0-beta.2 < 0.2.0-rc.1 < 0.2.0; 0.4 too
    echo "$1" | awk '{ v = $0; sub(/^[vV]/, "", v); pre = ""; i = index(v, "-"); if (i) { pre = substr(v, i + 1); v = substr(v, 1, i - 1) }
        n = split(v, m, "."); rank = 3; num = 0
        if (pre != "") { k = pre; j = index(pre, "."); if (j) { k = substr(pre, 1, j - 1); num = substr(pre, j + 1) + 0 }
            rank = (k == "rc") ? 2 : (k == "beta") ? 1 : 0 }
        printf "%05d%05d%05d%d%05d\n", m[1] + 0, m[2] + 0, m[3] + 0, rank, num }'
}
# the newest release that has both files
REL=; best=
for tag in $(cut -d' ' -f1 "$TMP/files" | grep '^v[0-9]' | sort -u); do
    v=${tag#v}
    grep -q "^$tag nocry-rescue-$v\.uImage " "$TMP/files" && grep -q "^$tag nocryboot-$FAMILY-[0-9][0-9.]*\.bin " "$TMP/files" || continue
    k=$(vkey "$v"); if [ -z "$best" ] || [ "$k" \> "$best" ]; then best=$k; REL=$v; fi
done
[ -n "$REL" ] || die "no release with a rescue image and a loader for this board was found on GitHub"
set -- $(grep "^v$REL nocry-rescue-$REL\.uImage " "$TMP/files" | head -1); R_SIZE=$3; R_SHA=$4; R_URL=$5
set -- $(grep "^v$REL nocryboot-$FAMILY-[0-9][0-9.]*\.bin " "$TMP/files" | head -1); L_NAME=$2; L_SIZE=$3; L_SHA=$4; L_URL=$5
L_VER=${L_NAME#nocryboot-$FAMILY-}; L_VER=${L_VER%.bin}
case "$R_URL$L_URL" in *" "*) die "unexpected answer from GitHub" ;; esac
case "$R_URL" in "$DOWNLOADS"*) ;; *) [ -n "${NOCRY_API:-}" ] || die "the release points somewhere else than this project's downloads" ;; esac
case "$L_URL" in "$DOWNLOADS"*) ;; *) [ -n "${NOCRY_API:-}" ] || die "the release points somewhere else than this project's downloads" ;; esac
[ "$L_SIZE" = 24576 ] || die "the loader in the release is not 24 KiB"
[ "$R_SIZE" -gt 1048576 ] && [ "$R_SIZE" -le $(( ROOM_MIB * 1048576 - 4096 )) ] || die "the rescue image in the release does not fit the room for it ($R_SIZE bytes)"
if [ "$NOW_KIND" = nocryboot ] && [ "$(vkey "$NOW_VER")" \> "$(vkey "$L_VER")" ]; then
    die "this board already has nocryboot $NOW_VER, newer than the release's $L_VER"
fi
say "Release $REL: rescue image ($((R_SIZE >> 20)) MiB) and nocryboot $L_VER."

get() {     # get URL SIZE SHA256 FILE: kept only if size and SHA-256 are what the release says
    case "$1" in
      https://*|http://*) fetch "$1" "$4" || die "the download of $(basename "$4") failed" ;;
      *) cp "$1" "$4" || die "no such file: $1" ;;
    esac
    [ "$(bytes "$4")" = "$2" ] || die "the download of $(basename "$4") is incomplete ($(bytes "$4") of $2 bytes)"
    [ "$(sha < "$4")" = "$3" ] || die "the download of $(basename "$4") is damaged (its SHA-256 is not what the release says)"
}
say "Downloading and checking..."
get "$L_URL" "$L_SIZE" "$L_SHA" "$TMP/loader.new"
get "$R_URL" "$R_SIZE" "$R_SHA" "$TMP/rescue"
# what they are, beyond the checksum
set -- $(inspect "$TMP/loader.new")
[ "${1:-}" = nocryboot ] && [ "${2:-}" = "$L_VER" ] && [ "${3:-}" = "$FAMILY" ] || die "the downloaded loader is not nocryboot $L_VER for this board family (it is: $*)"
grep -qa "restarting in USB boot mode" "$TMP/loader.new" && grep -qa "ota:kernel2" "$TMP/loader.new" && grep -qa "lcm_id=" "$TMP/loader.new" || die "the downloaded loader is not a complete nocryboot"
[ "$(dd if="$TMP/rescue" bs=1 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')" = 27051956 ] &&
    [ "$(dd if="$TMP/rescue" bs=1 skip=32 count=12 2>/dev/null)" = nocry-rescue ] || die "the downloaded rescue image is not a nocry rescue image"
# the image's own length field must be that of the file (a loader that finds a shorter or longer one rejects it)
[ $(( $(dd if="$TMP/rescue" bs=1 skip=12 count=4 2>/dev/null | od -An -tu1 | awk '{ print $1 * 16777216 + $2 * 65536 + $3 * 256 + $4 }') + 64 )) = "$R_SIZE" ] || die "the downloaded rescue image is not complete"

# the factory loader: kept already, or to be kept now?
KEEP=
if [ "$NOW_KIND" = factory ]; then
    if rd $MARK_SECTOR 49 | tr -d '\000' | wc -c | grep -q '^ *0$'; then KEEP=1      # the place is unused
    else
        # something is there: fine only if it is this very loader, kept before
        m=$(rd $MARK_SECTOR 1 | tr '\000' '\n' | head -1)
        [ "$m" = "nocry:factory-loader sha256=$(sha < "$TMP/loader.now") size=24576" ] && [ "$(rd $KEPT_SECTOR 48 | sha)" = "$(sha < "$TMP/loader.now")" ] ||
            die "the place in the boot area where the factory loader is kept is not empty on this board"
    fi
fi
SAME=; cmp -s "$TMP/loader.now" "$TMP/loader.new" && SAME=1

# ---- 3. say it, ask -------------------------------------------------------------------------
say ""
say "About to:"
say "  - store the rescue system $REL behind both firmware slots (last $ROOM_MIB MiB of rootfs and rootfs2)"
[ -n "$KEEP" ] && say "  - keep Creality's loader: on the board, and as $KEEP_DIR/nocry-factory-loader.bin"
if [ -n "$SAME" ]; then say "  - leave the loader: nocryboot $L_VER is already installed"
else say "  - replace the boot loader ($NOW_TEXT) with nocryboot $L_VER"; fi
say "Firmware, settings, files and the identity record are not touched."
if [ -n "$CHECK" ]; then say ""; say "Checked only; nothing was changed."; rm -rf "$TMP"; exit 0; fi
if [ -z "$YES" ]; then
    printf '\nDo not switch the printer off while this runs (about a minute). Go ahead? [y/N] '
    read ans < /dev/tty 2>/dev/null || die "no terminal to ask at; run it with -y to go ahead without asking"
    case "$ans" in y|Y|yes|YES) ;; *) die "stopped at your request" ;; esac
fi

# ---- 4. the rescue image, twice -------------------------------------------------------------
CHANGED=1
mib=$(( (R_SIZE + 1048575) / 1048576 ))
for at in $ROOM_AT; do
    say "Writing the rescue image at $at MiB..."
    dd if="$TMP/rescue" of="$DISK" bs=1048576 seek="$at" conv=notrunc,fsync 2>/dev/null || die "writing the rescue image failed. The boot loader was not touched; the board starts as before"
    fresh
    [ "$(dd if="$DISK" bs=1048576 skip="$at" count="$mib" 2>/dev/null | head -c "$R_SIZE" | sha)" = "$R_SHA" ] ||
        die "the rescue image does not read back as written. The boot loader was not touched; the board starts as before"
done
say "Rescue image stored and read back, both copies."

# ---- 5. keep Creality's loader --------------------------------------------------------------
if [ -n "$KEEP" ]; then
    cp "$TMP/loader.now" "$KEEP_DIR/nocry-factory-loader.bin" 2>/dev/null && sync || say "(could not write $KEEP_DIR/nocry-factory-loader.bin; the copy on the board is the one that counts)"
    { printf 'nocry:factory-loader sha256=%s size=24576\n' "$(sha < "$TMP/loader.now")"; dd if=/dev/zero bs=512 count=1 2>/dev/null; } | head -c 512 > "$TMP/mark"
    [ "$(bytes "$TMP/mark")" = 512 ] || die "could not prepare the note for the kept loader"
    dd if="$TMP/loader.now" of="$DISK" bs=512 seek=$KEPT_SECTOR conv=notrunc,fsync 2>/dev/null &&
        dd if="$TMP/mark" of="$DISK" bs=512 seek=$MARK_SECTOR conv=notrunc,fsync 2>/dev/null || die "keeping the factory loader failed. The boot loader was not touched; the board starts as before"
    fresh
    rd $KEPT_SECTOR 48 | cmp -s - "$TMP/loader.now" && rd $MARK_SECTOR 1 | cmp -s - "$TMP/mark" || die "the kept factory loader does not read back right. The boot loader was not touched; the board starts as before"
    say "Creality's loader is kept on the board."
fi

# ---- 6. the loader ---------------------------------------------------------------------------
if [ -z "$SAME" ]; then
    # once more, right before: the loader on the board is still the one that was looked at
    rd $SPL_SECTOR $SPL_SECTORS | cmp -s - "$TMP/loader.now" || die "the loader on the board changed while this ran; not writing over it"
    say "Writing nocryboot $L_VER. Do not switch off..."
    put() { dd if="$1" of="$DISK" bs=512 seek=$SPL_SECTOR conv=notrunc,fsync 2>/dev/null; fresh; rd $SPL_SECTOR $SPL_SECTORS | cmp -s - "$1"; }
    if ! put "$TMP/loader.new" && ! put "$TMP/loader.new"; then
        say "The loader does not read back as written. Putting back the one that was there..."
        if put "$TMP/loader.now" || put "$TMP/loader.now"; then die "nocryboot could not be written; the loader that was there is back and reads back right. The board starts as before"; fi
        printf '\nnocry install: THE BOOT LOADER COULD NOT BE WRITTEN AND NOT BE PUT BACK.\nDo not switch the printer off. Run this script again; if that fails too, the board\nneeds USB boot mode (BOOT button) and nocry on a computer: nocry loader --factory --yes\n' >&2
        exit 1
    fi
    say "nocryboot $L_VER installed and read back."
fi
rm -rf "$TMP"; sync
say ""
say "Done. To start the rescue system: switch the printer off, put a finger on the screen,"
say "switch it on. Without the finger the printer starts as before."
