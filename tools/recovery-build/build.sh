#!/usr/bin/env bash
# Reproducible build of the nocry recovery image (RAM-only Linux for Creality Nebula/X2000E):
#   OUT/nocry-recovery.uImage   gzip uImage @0x80010000: Linux 6.6 (k1-debian) + built-in initramfs
#                               (busybox, adbd over FunctionFS, ACM console, e2fsprogs, util-linux)
# Boot it without touching the eMMC:  nocry fastboot && fastboot boot OUT/nocry-recovery.uImage
#
# Usage: tools/recovery-build/build.sh [WORKDIR] [OUTDIR]      (needs docker + git; ~10 GB in WORKDIR)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
# The same commands on Linux and macOS (no GNU-only options), and the same compilers on every
# computer: the build containers are x86-64, emulated where the computer is not.
abspath() { case "$1" in /*) printf '%s\n' "$1" ;; *) printf '%s\n' "$PWD/$1" ;; esac; }
sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
export DOCKER_DEFAULT_PLATFORM=${DOCKER_DEFAULT_PLATFORM:-linux/amd64}
WORK=$(abspath "${1:-$HOME/nocry-work}")
OUT=$(abspath "${2:-$HERE/out}")
LINUX_REPO=https://github.com/k1-debian/linux
LINUX_BRANCH=master-k1
LINUX_SHA=043f9fc7090512e504386a3866aa5a9d0b6af737
LINUX_REV=043f9fc70        # 2026-09-28 "implement support for Creality K1 contol board" (Linux 6.6.18)
BR_REPO=https://gitlab.com/buildroot.org/buildroot.git
BR_TAG=2025.02.20

mkdir -p "$WORK" "$OUT"
# The Linux source has files whose names differ only in upper and lower case. On a file system
# that does not tell them apart (the macOS default) the checkout is silently wrong.
rm -f "$WORK/.Case-Test" && : > "$WORK/.case-test"
if [ -e "$WORK/.Case-Test" ]; then
  rm -f "$WORK/.case-test"
  cat >&2 <<EOT
error: $WORK is on a file system that ignores upper and lower case in file names; the Linux
kernel cannot be built there. Give a work folder on a case-sensitive one. On macOS:
  hdiutil create -type SPARSE -fs 'Case-sensitive APFS' -size 40g -volname nocry-work ~/nocry-work.sparseimage
  hdiutil attach ~/nocry-work.sparseimage
  $0 /Volumes/nocry-work
EOT
  exit 1
fi
rm -f "$WORK/.case-test"
docker build -q -t nocry-recovery-build "$HERE" >/dev/null
# /repo/host: nocry's own source, built for the board into the rescue system (post-build.sh);
# /images: the loader and boot logo images it installs from there, when they have been built
REPO=$(cd "$HERE/../.." && pwd)
IMAGES=$WORK/board-images
rm -rf "$IMAGES"; mkdir -p "$IMAGES"
BURNER_OUT=${BURNER_OUT:-$HERE/../burner-build/out}; NOCRYBOOT_OUT=${NOCRYBOOT_OUT:-$HERE/../nocryboot-build/out}; RTOS_OUT=${RTOS_OUT:-$HERE/../rtos-build/out}
if [ -f "$BURNER_OUT/spl-x2000e/spl-x2000e.bin" ] && [ -f "$NOCRYBOOT_OUT/nocryboot-nebula.bin" ] && [ -f "$RTOS_OUT/nocry-rtos.bin" ]; then
  mkdir -p "$IMAGES/spl-x2000e" "$IMAGES/nocryboot" "$IMAGES/nocry-rtos"
  cp "$BURNER_OUT/spl-x2000e/spl-x2000e.bin" "$IMAGES/spl-x2000e/"
  cp "$NOCRYBOOT_OUT/nocryboot-nebula.bin" "$NOCRYBOOT_OUT/nocryboot-k1.bin" "$IMAGES/nocryboot/"
  cp "$RTOS_OUT/nocry-rtos.bin" "$IMAGES/nocry-rtos/nocry-rtos-nebula.bin"
  [ -f "$RTOS_OUT/nocry-rtos-k1.bin" ] && cp "$RTOS_OUT/nocry-rtos-k1.bin" "$IMAGES/nocry-rtos/"
else
  echo "note: loader and boot logo images are not built yet; the rescue system will not be able to install them (tools/build-images.sh builds everything in order)"
fi
run() { docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$WORK:/w" -v "$WORK/buildroot:/br" -v "$HERE:/nocry:ro" -v "$REPO/host:/repo/host:ro" -v "$IMAGES:/images:ro" -w /w nocry-recovery-build bash -c "$1"; }

# --- sources
if [ ! -d "$WORK/linux" ]; then
  git init -q "$WORK/linux" && git -C "$WORK/linux" remote add origin "$LINUX_REPO"
  # fetch the pinned commit itself, so the build does not break when the branch moves
  git -C "$WORK/linux" fetch -q --depth 1 origin "$LINUX_SHA" && git -C "$WORK/linux" checkout -q FETCH_HEAD
fi
[ "$(git -C "$WORK/linux" rev-parse --short=9 HEAD)" = "$LINUX_REV" ] || { echo "linux is not at $LINUX_REV" >&2; exit 1; }
# Never discard work: a clean tree gets the patch; a tree that already matches it is used as is;
# anything else must be saved first (save-kernel-patch.sh).
KP="$HERE/patches/0001-nocry-nebula-recovery-dt.patch"
git -C "$WORK/linux" add -A -N
if git -C "$WORK/linux" diff --quiet; then
  git -C "$WORK/linux" apply "$KP"
elif ! git -C "$WORK/linux" diff | cmp -s - "$KP"; then
  echo "error: $WORK/linux has changes that are not in $KP; run save-kernel-patch.sh first" >&2
  exit 1
fi
[ -d "$WORK/buildroot" ] || git clone -q --depth 1 --branch "$BR_TAG" "$BR_REPO" "$WORK/buildroot"

# --- initramfs (Buildroot)
# a changed defconfig (e.g. arch/ABI) needs a clean Buildroot build; packages are not rebuilt otherwise
if ! cmp -s "$HERE/buildroot.defconfig" "$WORK/buildroot/.nocry-defconfig-built" 2>/dev/null && [ -d "$WORK/buildroot/output" ]; then
  echo "buildroot.defconfig changed: clean Buildroot build (~15 min)"
  run "cd /br && make clean >/dev/null"
fi
cp "$HERE/buildroot.defconfig" "$WORK/buildroot/configs/nocry_recovery_defconfig"
# Buildroot's host tools embed absolute paths: always build it at /br
run "cd /br && make -s NOCRY_RECOVERY=/nocry nocry_recovery_defconfig && make NOCRY_RECOVERY=/nocry -j\$(nproc) > /w/buildroot-build.log 2>&1" \
  || { tail -30 "$WORK/buildroot-build.log"; exit 1; }
cp "$HERE/buildroot.defconfig" "$WORK/buildroot/.nocry-defconfig-built"

# --- kernel with the initramfs built in
run "set -e; mkdir -p kout
  make -s -C linux ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- O=/w/kout k1_defconfig
  KSRC=/w/linux KOUT=/w/kout sh /nocry/kernel-config.sh /br/output/images/rootfs.cpio
  make -s -C linux ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- O=/w/kout olddefconfig
  make -C linux ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- O=/w/kout -j\$(nproc) vmlinux > /w/kernel-build.log 2>&1
  cd kout
  mipsel-linux-gnu-objcopy -O binary -R .comment -R .note -R .reginfo vmlinux vmlinux.bin
  gzip -9nf vmlinux.bin
  entry=0x\$(mipsel-linux-gnu-nm vmlinux | awk '\$3==\"kernel_entry\"{print substr(\$1,9)}')
  mkimage -A mips -O linux -T kernel -C gzip -a 0x80010000 -e \$entry -n nocry-recovery -d vmlinux.bin.gz nocry-recovery.uImage >/dev/null" \
  || { tail -30 "$WORK/kernel-build.log"; exit 1; }
cp "$WORK/kout/nocry-recovery.uImage" "$OUT/"

# --- RESCUE=1: also nocry-rescue (docs/nocry-rescue.md): its own kernel configuration and an
# initramfs with the recovery's contents plus Wi-Fi firmware and network tools
if [ "${RESCUE:-0}" = 1 ]; then
  cat "$HERE/buildroot.defconfig" "$HERE/buildroot-rescue.fragment" > "$WORK/buildroot/configs/nocry_rescue_defconfig"
  if ! cmp -s "$WORK/buildroot/configs/nocry_rescue_defconfig" "$WORK/buildroot/.nocry-rescue-defconfig-built" 2>/dev/null && [ -d "$WORK/buildroot/output-rescue" ]; then
    echo "rescue defconfig changed: clean Buildroot build (~15 min)"
    run "cd /br && make O=/br/output-rescue clean >/dev/null"
  fi
  run "cd /br && make -s O=/br/output-rescue NOCRY_RECOVERY=/nocry nocry_rescue_defconfig && make O=/br/output-rescue NOCRY_RECOVERY=/nocry -j\$(nproc) > /w/buildroot-rescue-build.log 2>&1" \
    || { tail -30 "$WORK/buildroot-rescue-build.log"; exit 1; }
  cp "$WORK/buildroot/configs/nocry_rescue_defconfig" "$WORK/buildroot/.nocry-rescue-defconfig-built"
  run "set -e; mkdir -p kout-rescue
    make -s -C linux ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- O=/w/kout-rescue k1_defconfig
    KSRC=/w/linux KOUT=/w/kout-rescue sh /nocry/kernel-config.sh /br/output-rescue/images/rootfs.cpio rescue
    make -s -C linux ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- O=/w/kout-rescue olddefconfig
    make -C linux ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- O=/w/kout-rescue -j\$(nproc) vmlinuz > /w/kernel-rescue-build.log 2>&1
    cd kout-rescue
    # a kernel that unpacks itself (as the firmware's own kernels do): nocryboot's first stage
    # starts it straight from the eMMC and cannot unpack anything. The name is what that
    # stage looks for; load and start address are where the unpacker was linked.
    mipsel-linux-gnu-objcopy -O binary -R .comment -R .note -R .reginfo vmlinuz vmlinuz.bin
    entry=\$(mipsel-linux-gnu-readelf -h vmlinuz | awk '/Entry point/{print \$4}')
    mkimage -A mips -O linux -T kernel -C none -a \$entry -e \$entry -n nocry-rescue -d vmlinuz.bin nocry-rescue.uImage >/dev/null" \
    || { grep -n 'error\|Error' "$WORK/kernel-rescue-build.log" | head -30; tail -15 "$WORK/kernel-rescue-build.log"; exit 1; }
  cp "$WORK/kout-rescue/nocry-rescue.uImage" "$OUT/"
fi
cd "$OUT" && ls -l nocry-recovery.uImage && sha256 nocry-recovery.uImage
