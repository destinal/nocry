#!/usr/bin/env bash
# Gets everything nocry puts on a board (USB boot stages, the nocryboot loader, the boot picture
# program, the recovery Linux, nocry-rescue) into ./images, where a nocry built from this tree
# looks for it.
#
# By default the set that GitHub built from this repository's sources is downloaded and checked:
# compiling it takes about an hour and needs Docker. To compile it yourself instead:
#
#   tools/get-images.sh            download the set built from the newest sources (main)
#   tools/get-images.sh v0.1.0-alpha.4     download the set of that release
#   tools/get-images.sh --build [...]      build locally (tools/build-images.sh, same arguments)
#
# Downloads come from this project's GitHub releases; the "board-images" one is replaced by the
# `board-images` workflow whenever the sources of the images change. Needs curl and tar.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
REPO=${NOCRY_REPO:-destinal/nocry}
BASE="https://github.com/$REPO/releases/download"

case "${1:-}" in
  --build) shift; exec "$HERE/build-images.sh" "$@" ;;
  -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
esac
sha256() { if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
for tool in curl tar; do
  command -v "$tool" >/dev/null 2>&1 || { echo "error: $tool is needed and was not found" >&2; exit 1; }
done

tag=${1:-board-images}
case "$tag" in
  board-images) file=nocry-board-images.tar.gz ;;
  v*) file=nocry-board-images-${tag#v}.tar.gz ;;
  *) echo "error: give a release tag such as v0.1.0-alpha.4, or nothing for the newest set" >&2; exit 2 ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "downloading $file ($tag)"
if ! curl -fL --retry 3 --progress-bar -o "$tmp/$file" "$BASE/$tag/$file"; then
  echo "error: could not download $BASE/$tag/$file" >&2
  echo "       (no such release, or no connection). To build the images yourself: $0 --build" >&2
  exit 1
fi
mkdir "$tmp/x" && tar xzf "$tmp/$file" -C "$tmp/x"
# every file is listed with its checksum inside the set; a damaged or cut-off download fails here
[ -s "$tmp/x/SHA256SUMS" ] || { echo "error: the downloaded set has no checksum list" >&2; exit 1; }
(cd "$tmp/x" && sha256 -c SHA256SUMS >/dev/null) || { echo "error: the downloaded set is damaged (checksums do not match); nothing was installed" >&2; exit 1; }
[ -f "$tmp/x/spl-x2000e/spl-x2000e.bin" ] || { echo "error: the downloaded set is incomplete" >&2; exit 1; }

rm -rf "$ROOT/images.new" && mv "$tmp/x" "$ROOT/images.new"
rm -rf "$ROOT/images" && mv "$ROOT/images.new" "$ROOT/images"
echo "board images are in $ROOT/images:"
(cd "$ROOT/images" && find . -type f ! -name SHA256SUMS ! -name SOURCE_COMMIT | sort | sed 's/^\.\//  /')
if [ -f "$ROOT/images/SOURCE_COMMIT" ]; then
  built=$(cat "$ROOT/images/SOURCE_COMMIT")
  echo "built from commit ${built:0:9}"
  here=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || true)
  if [ -n "$here" ] && [ "$here" != "$built" ] && ! git -C "$ROOT" diff --quiet "$built" "$here" -- tools rtos 2>/dev/null; then
    echo "note: the image sources in this checkout differ from that commit; to match them exactly: $0 --build"
  fi
fi
