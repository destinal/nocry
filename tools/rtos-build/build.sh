#!/bin/sh
# Builds nocry-rtos (rtos/) into tools/rtos-build/out/nocry-rtos.bin, an image for the rtos
# partition. Uses the compiler of the burner-build Docker image (tools/burner-build).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT=${1:-$HERE/out}
IMG=nocry-uboot-build
docker image inspect $IMG >/dev/null 2>&1 || docker build -q -t $IMG "$ROOT/tools/burner-build" >/dev/null
mkdir -p "$OUT"
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT/rtos:/src:ro" -v "$OUT:/out" $IMG sh -euc '
  CF="-EL -mips32r2 -mno-abicalls -fno-pic -G0 -msoft-float -ffreestanding -fno-builtin -nostdlib -Os -Wall -Werror"
  mipsel-linux-gnu-gcc $CF -c /src/start.S -o /out/start.o
  mipsel-linux-gnu-gcc $CF -c /src/main.c -o /out/main.o
  mipsel-linux-gnu-gcc $CF -c /src/display.c -o /out/display.o
  mipsel-linux-gnu-gcc $CF -c /src/cpu.S -o /out/cpu.o
  mipsel-linux-gnu-gcc $CF -c /src/pictures.c -o /out/pictures.o
  mipsel-linux-gnu-ld -EL -T /src/link.ld -Map /out/nocry-rtos.map -o /out/nocry-rtos.elf /out/start.o /out/main.o /out/display.o /out/cpu.o /out/pictures.o
  mipsel-linux-gnu-objcopy -O binary /out/nocry-rtos.elf /out/nocry-rtos.bin
  mipsel-linux-gnu-objdump -d /out/nocry-rtos.elf > /out/nocry-rtos.lst
'
# Pictures: appended behind the code; img_end in the header (offset 0x14) is set behind them.
# PICTURES="--from-stock <zero.bin>" (or any mkpictures.py arguments) replaces nocry's own.
P="$ROOT/rtos/pictures"
: "${PICTURES:=--default $P/default.png Nebula\ Pad=$P/nebula-pad.png F003=$P/f003.png F004=$P/f004.png F005=$P/f005.png}"
eval "python3 \"$HERE/mkpictures.py\" -o \"$OUT/pictures.bin\" $PICTURES"
python3 - "$OUT/nocry-rtos.bin" "$OUT/pictures.bin" <<'PY'
import struct, sys
code = open(sys.argv[1], 'rb').read(); pics = open(sys.argv[2], 'rb').read()
BASE, WORK = 0x86000000, 0x86060000
start, end = struct.unpack_from('<II', code, 0x10)
assert start == BASE and 0 <= end - BASE - len(code) < 16, 'unexpected header'
code += b'\\0' * (end - BASE - len(code))      # objcopy stops at the last byte of data
img = bytearray(code + pics)
img += b'\0' * (-len(img) % 512)
if BASE + len(img) > WORK:
    sys.exit('pictures too large: %d bytes over' % (BASE + len(img) - WORK))
struct.pack_into('<I', img, 0x14, BASE + len(img))
open(sys.argv[1], 'wb').write(img)
PY
ls -l "$OUT/nocry-rtos.bin"
