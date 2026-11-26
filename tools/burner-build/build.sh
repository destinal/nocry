#!/usr/bin/env bash
# Reproducible build of nocry's X2000(E)/X2000-V12 USB-boot images from GPL source.
#
# Default (k1-debian/u-boot, SoC x2000_v12 = what Creality's Nebula/K1 boards are):
#   OUT/spl-x2000e/spl-x2000e.bin            stage1: nocry_spl_x2000_v12 SPL + nocry-splhdr header
#   OUT/nocry_ums_x2000_v12/u-boot.bin       stage2: eMMC as USB disk, read-write
#   OUT/nocry_ums_ro_x2000_v12/u-boot.bin    stage2: eMMC as USB disk, read-only
#   OUT/nocry_fb_x2000_v12/u-boot.bin        stage2: Android fastboot for the eMMC
#
# LEGACY=1 additionally builds the old uboot-xburst (non-V12 "x2000") targets:
#   burner_x2000_lpddr2 (Ingenic cloner burner), nocry_ums_x2000, nocry_ums_ro_x2000
#
# Usage: tools/burner-build/build.sh [OUTDIR]        (needs docker + git)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(realpath -m "${1:-$HERE/out}")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

K1DEB_REPO=https://github.com/k1-debian/u-boot
K1DEB_REV=271fbe7           # 2026-09-28 "Creality K1 Support"; Ingenic ingenic-master + x2000_v12
XBURST_REPO=https://github.com/Ingenic-community/uboot-xburst
XBURST_REV=1060b51          # 2023-02-27; closest public match to ballaswag's burner blob (legacy only)

docker build -q -t nocry-uboot-build "$HERE" >/dev/null
mkdir -p "$OUT"

build() {   # build <srcdir> <target> <outdir>
  local src=$1 t=$2 out=$3
  mkdir -p "$out"
  docker run --rm -v "$src:/src:ro" -v "$out:/out" nocry-uboot-build bash -c "
    cp -a /src /w && cd /w &&
    make CROSS_COMPILE=mipsel-linux-gnu- $t -j\$(nproc) > /out/build.log 2>&1 &&
    cp u-boot u-boot.bin System.map /out/ && cp spl/u-boot-spl spl/u-boot-spl.bin /out/ &&
    chown -R $(id -u):$(id -g) /out" || { echo "build of $t failed, see $out/build.log" >&2; exit 1; }
}

# --- k1-debian (x2000_v12) ---------------------------------------------------
git clone -q --depth 50 "$K1DEB_REPO" "$WORK/k1deb" && git -C "$WORK/k1deb" checkout -q "$K1DEB_REV"
cp -a "$WORK/k1deb" "$WORK/k1deb-nocry"
for p in "$HERE"/patches/k1deb/*.patch; do patch -s -d "$WORK/k1deb-nocry" -p1 < "$p"; done

# stage1: classic V12 SPL (nocry_spl_x2000_v12 = burner_x2000 without the SPL-side
# USB loop, which would answer bootrom requests before DDR init) + generated header
build "$WORK/k1deb-nocry" nocry_spl_x2000_v12 "$OUT/spl-x2000e"
python3 -I "$HERE/../splhdr/nocry_splhdr.py" build \
  --profile "$HERE/../splhdr/profiles/creality-x2000e.json" \
  --spl "$OUT/spl-x2000e/u-boot-spl.bin" -o "$OUT/spl-x2000e/spl-x2000e.bin"

# stage2: USB mass storage, 52 MHz MSC0 (PIO; SDMA corrupts reads on this host) +
# 128 KiB buffers, no cloner/eFuse code
# stage2: fastboot (f_nocry_fb.c, patch 0002), same MSC0 setup.
# Keep every stage2 under 0x44000 bytes: a 278840-byte image did not start on hardware.
for t in nocry_ums_x2000_v12 nocry_ums_ro_x2000_v12 nocry_fb_x2000_v12; do
  build "$WORK/k1deb-nocry" "$t" "$OUT/$t"
  sz=$(stat -c %s "$OUT/$t/u-boot.bin")
  if [ "$sz" -ge $((0x44000)) ]; then
    echo "error: $t is $sz bytes; stage2 must stay below 0x44000 (bigger images don't start)" >&2; exit 1
  fi
  if grep -qiE ' (f_cloner|clmd_|efuse|cpu_burn|init_seboot)' "$OUT/$t/System.map"; then
    echo "error: $t contains cloner/eFuse symbols" >&2; exit 1
  fi
done

# --- legacy uboot-xburst (non-V12) -------------------------------------------
if [ "${LEGACY:-0}" = 1 ]; then
  git clone -q "$XBURST_REPO" "$WORK/xburst" && git -C "$WORK/xburst" checkout -q "$XBURST_REV"
  cp "$WORK/k1deb/include/linux/compiler-gcc7.h" "$WORK/xburst/include/linux/"
  for p in "$HERE"/patches/xburst/*.patch; do patch -s -d "$WORK/xburst" -p1 < "$p"; done
  for t in burner_x2000_lpddr2 nocry_ums_x2000 nocry_ums_ro_x2000; do build "$WORK/xburst" "$t" "$OUT/$t"; done
fi

cd "$OUT" && sha256sum spl-x2000e/spl-x2000e.bin nocry_ums_*/u-boot.bin nocry_fb_*/u-boot.bin
