#!/usr/bin/env bash
# Builds everything nocry puts on a board, from this repository's sources and patches, into the
# places where a nocry built from the same tree looks for it:
#
#   tools/burner-build/out      first and second stage for USB boot mode (SPL, U-Boot)
#   tools/nocryboot-build/out   the nocryboot loader for the eMMC
#   tools/rtos-build/out        the boot logo program, for both board families
#   tools/recovery-build/out    the recovery Linux; with --rescue also nocry-rescue
#
# None of these are in git: they are compiled for the board's processor from upstream U-Boot,
# Linux and Buildroot at pinned revisions (downloaded by the scripts) plus the patches here.
#
# Usage: tools/build-images.sh [--rescue] [WORKDIR]
#   WORKDIR  where the Linux and Buildroot trees are kept and built (default ~/nocry-work,
#            about 10 GB; must be on a file system that tells upper and lower case apart)
# Needs: Docker, git, python3 with Pillow. The first run takes about an hour, most of it the
# recovery Linux; later runs only redo what changed.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RESCUE=0; WORK=
for a in "$@"; do
  case "$a" in
    --rescue) RESCUE=1 ;;
    -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "unknown option $a" >&2; exit 2 ;;
    *) WORK=$a ;;
  esac
done
for tool in docker git python3; do
  command -v "$tool" >/dev/null 2>&1 || { echo "error: $tool is needed and was not found" >&2; exit 1; }
done
docker info >/dev/null 2>&1 || { echo "error: Docker is installed but not running (or this user may not use it)" >&2; exit 1; }

step() { printf '\n== %s\n' "$1"; }
step "USB boot stages (SPL, U-Boot)";        "$HERE/burner-build/build.sh"
step "nocryboot loader";                     "$HERE/nocryboot-build/build.sh"
step "boot logo program (Nebula Pad)";    "$HERE/rtos-build/build.sh"
step "boot logo program (K1 family)";     VARIANT=k1 "$HERE/rtos-build/build.sh"
step "recovery Linux$([ $RESCUE = 1 ] && echo ' and nocry-rescue')"
RESCUE=$RESCUE "$HERE/recovery-build/build.sh" ${WORK:+"$WORK"}
printf '\nAll board images are built. nocry built from this tree (host/, gui/) finds them by itself.\n'
