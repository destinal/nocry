#!/bin/sh
# Builds the register-capture program as a uImage the board's loader starts instead of Linux.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
OUT=${1:-$ROOT/tools/rtos-build/out}
mkdir -p "$OUT"
docker run --rm -u "$(id -u):$(id -g)" -v "$HERE:/src:ro" -v "$OUT:/out" nocry-uboot-build sh -euc '
  CF="-EL -mips32r2 -mno-abicalls -fno-pic -G0 -msoft-float -ffreestanding -fno-builtin -nostdlib -Os -Wall -Werror"
  mipsel-linux-gnu-gcc $CF -c /src/dump.c -o /out/rd-dump.o
  mipsel-linux-gnu-ld -EL -Ttext 0x80f00000 -e dump_main -o /out/regdump.elf /out/rd-dump.o
  mipsel-linux-gnu-objcopy -O binary -j .text -j .rodata -j .data /out/regdump.elf /out/regdump.bin
'
python3 - "$OUT/regdump.bin" "$OUT/regdump.uImage" <<'PY'
import sys, struct, zlib, time
d = open(sys.argv[1], 'rb').read()
d += b'\0' * (-len(d) % 512)
name = b'nocry regdump'.ljust(32, b'\0')
h = struct.pack('>IIIIIIIBBBB', 0x27051956, 0, int(time.time()), len(d), 0x80f00000, 0x80f00000, zlib.crc32(d) & 0xffffffff, 5, 5, 2, 0) + name
h = h[:4] + struct.pack('>I', zlib.crc32(h) & 0xffffffff) + h[8:]
open(sys.argv[2], 'wb').write(h + d)
print(sys.argv[2], len(h + d), 'bytes')
PY
