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
  mipsel-linux-gnu-ld -EL -T /src/link.ld -Map /out/nocry-rtos.map -o /out/nocry-rtos.elf /out/start.o /out/main.o /out/display.o
  mipsel-linux-gnu-objcopy -O binary /out/nocry-rtos.elf /out/nocry-rtos.bin
  mipsel-linux-gnu-objdump -d /out/nocry-rtos.elf > /out/nocry-rtos.lst
'
# the header's img_end is sector-aligned; make the file as long as what the SPL loads
truncate -s %512 "$OUT/nocry-rtos.bin"
ls -l "$OUT/nocry-rtos.bin"
