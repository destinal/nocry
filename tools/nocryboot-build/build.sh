#!/usr/bin/env bash
# Reproducible build of nocryboot's first stage (SPL): the open replacement for the loader
# that Creality's X2000E boards start from their eMMC.
#
#   OUT/nocryboot-nebula.bin   Nebula Pad family (console on UART4, pins PA2/PA3)
#   OUT/nocryboot-k1.bin       K1 family         (console on UART2, pins PD30/PD31)
#
# Each file is the complete loader as it sits on the eMMC at byte 0x4400 (header with the
# memory settings + code, 24 KiB). Try one without installing it:
#   nocry run-spl OUT/nocryboot-nebula.bin        (board in USB boot mode)
#
# Source: k1-debian/u-boot (Ingenic's U-Boot 2013.07 tree with x2000_v12 support) plus
# patches/*.patch. See docs/nocryboot.md.
#
# Usage: tools/nocryboot-build/build.sh [OUTDIR]        (needs docker + git)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(realpath -m "${1:-$HERE/out}")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

REPO=https://github.com/k1-debian/u-boot
REV=271fbe7           # same revision as tools/burner-build

docker build -q -t nocry-uboot-build "$HERE/../burner-build" >/dev/null
mkdir -p "$OUT"
git clone -q --depth 50 "$REPO" "$WORK/src" && git -C "$WORK/src" checkout -q "$REV"
for p in "$HERE"/patches/*.patch; do patch -s -d "$WORK/src" -p1 < "$p"; done

# doortest: a loader that does nothing but restart the board into USB boot mode; start it
# with `nocry run-spl` to check that a board's boot ROM honours the recovery door.
for fam in nebula k1 doortest; do
  t=nocryboot_$fam; o="$OUT/$t"; mkdir -p "$o"
  docker run --rm -v "$WORK/src:/src:ro" -v "$o:/out" nocry-uboot-build bash -c "
    cp -a /src /w && cd /w &&
    make CROSS_COMPILE=mipsel-linux-gnu- $t -j\$(nproc) > /out/build.log 2>&1 &&
    cp spl/u-boot-spl spl/u-boot-spl.map spl/u-boot-spl-pad.bin /out/ &&
    mipsel-linux-gnu-objdump -d spl/u-boot-spl > /out/u-boot-spl.dis &&
    chown -R $(id -u):$(id -g) /out" || { echo "build of $t failed, see $o/build.log" >&2; exit 1; }
  cp "$o/u-boot-spl-pad.bin" "$OUT/nocryboot-$fam.bin"
  python3 -I - "$OUT/nocryboot-$fam.bin" "$o/u-boot-spl.dis" <<'PY'
import sys, re
b = open(sys.argv[1], 'rb').read(); dis = open(sys.argv[2]).read()
assert len(b) == 24576, 'loader must be exactly 24 KiB, is %d' % len(b)
assert b[:4] == b'LPSM' and b[0x100:0x104] == b'INGE', 'header missing'
# nocryboot must not touch the eFuse block (0xb3540000) at all
assert not re.search(r'\blui\s+\w+,0xb354\b', dis), 'the loader references the eFuse block'
assert b'lcm_id=' in b and b'sn_mac' in b and b'ota:kernel2' in b
assert b'restarting in USB boot mode' in b
PY
done
cd "$OUT" && sha256sum nocryboot-nebula.bin nocryboot-k1.bin nocryboot-doortest.bin
