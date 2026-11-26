# nocry-rtos

An open replacement for the image in a Nebula board's `rtos` partition (Creality's is a
closed FreeRTOS program that shows the boot logo). Background: `docs/rtos-partition.md`.

Status: **works on a Nebula Pad** (2026-11-26, slot B, firmware 1.1.0.30). With this image in
the rtos partition the board shows a picture at power-on, the loader gets its answer
(`lcm_id=0`), and Creality's Linux starts, takes over the display and reaches the normal
interface.

What it does, in the order the stock image does it (`display.c`): opens the clock gates,
sets the PWM and pixel clocks, switches the panel pins, programs the display controller,
powers the 480x272 panel up, shows one frame through the layer composer, switches the
backlight on, then hands back to the loader (`main.c`). The sequence and values were taken
from a trace of the stock image (`tools/rtos-analysis/`), not guessed.

The hardware state it hands over was compared with the stock image's on the board, register
by register (1048 registers); what still differs is listed and explained in
`docs/rtos-partition.md`.

Boot picture: chosen by the model field of the board's `sn_mac` record, as the stock image
chooses its logo (`pictures.c`). The pictures are packed by `tools/rtos-build/mkpictures.py`
and appended to the image by the build. nocry's own pictures (`rtos/pictures/`, drawn by
`make_default_pictures.py`) are used by default; `PICTURES="--from-stock <zero.bin>"
tools/rtos-build/build.sh` takes the logo out of a firmware's own rtos image instead, so a
board can keep its original look without that artwork being part of nocry.

Not done yet: other panels (480x800 boards).

The CPU core is treated as the stock image treats it: FPU and MSA are switched on, their
control registers read (FCSR 0x000c0000, MSACSR 0 on this board) and switched off again;
before reporting back, Status and EBase are restored and the first-level, second-level and
instruction caches are written back and emptied.

Build: `tools/rtos-build/build.sh` (needs Docker) gives `tools/rtos-build/out/nocry-rtos.bin`.

If an image hangs, the board stops before Linux. Recover through USB boot mode: switch the
active slot back or write the firmware's own rtos image. The image keeps a text log in RAM
at physical 0x06e00000 (starts with `NCRTLOG1`); after BOOT+RESET, `nocry fastboot` and
`fastboot fetch ram ram.bin` retrieve it.
