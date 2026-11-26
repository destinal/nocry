#!/bin/sh
# Regenerate patches/0001 from a kernel work tree, including new (untracked) files.
# usage: save-kernel-patch.sh [linux work tree]   (default ~/nocry-work/linux)
set -e
L=${1:-$HOME/nocry-work/linux}
HERE=$(cd "$(dirname "$0")" && pwd)
git -C "$L" add -A -N
git -C "$L" diff > "$HERE/patches/0001-nocry-nebula-recovery-dt.patch"
git -C "$L" diff --stat | tail -1
