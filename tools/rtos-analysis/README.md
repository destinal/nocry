# Tracing Creality's rtos image

`emu.py` runs the stock image of a Nebula Pad's `rtos` partition in the Unicorn CPU emulator
and prints every hardware register access in order, together with the image's own log
messages. It is how the set-up sequence in `rtos/display.c` was established.

    pip install --user unicorn
    python3 emu.py <rtos image> <regdump>            # CPM (clock) registers seeded from a running system
    GATED=1 python3 emu.py <rtos image> <regdump>    # all clock gates closed first: shows which the image opens

`<rtos image>` is the start of the `rtos` partition (or the firmware's `zero.bin`).
`<regdump>` is a text file with `address value` lines (`10000014 0x07C0484D`) of the CPM
registers 0x10000000-0x100000ff from a running stock system; the image reads the PLL
settings from them.

How it works, and its limits:

- The scheduler is not emulated. Thread functions run one after another; when one sleeps, the
  threads created so far run, as they would on the board. The serial-console thread is skipped.
- Waiting for the display's "frame done" interrupt is answered by running the image's own
  interrupt handler with that flag set.
- Cache maintenance, FPU and MSA set-up are skipped: the emulated CPU (74Kf) lacks them.
- Peripheral registers are a plain dictionary (plus GPIO set/clear and PWM enable semantics),
  so values the hardware would change by itself are not modelled.
- Addresses in `emu.py` and `stubs_extra.py` are for the retail 1.1.0.30 image
  (sha256 of the first 0x67948 bytes starts f0ab5db5120c8d69).

## Register capture on the board

`regdump/` builds a tiny program packed as a kernel image. Written to a slot's kernel partition,
the loader starts it right after the rtos image instead of Linux; it copies about a thousand
hardware registers into RAM and samples the display status for a while. After BOOT+RESET:

    nocry fastboot && fastboot fetch ram ram.bin
    python3 regdump/capread.py ram.bin > capture.txt

Run it once behind the stock rtos image and once behind another and compare the two files.
Restore the kernel afterwards (`nocry copy-slot`). Because the program is so small, the loader
starts it a few milliseconds after the rtos image ends, much sooner than a real kernel.
