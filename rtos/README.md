# nocry-rtos

An open replacement for the image in a Nebula board's `rtos` partition (Creality's is a
closed FreeRTOS program that shows the boot logo). Background: `docs/rtos-partition.md`.

Status: **stage 1, not yet run on a board.** The image has the header the board's loader
checks and performs the hand-over the loader waits for (report panel id 0, mark itself
done, put its CPU core to sleep). It shows no picture, so with it the screen stays dark
until Linux draws.

Planned next: display and backlight setup and a boot picture, using the X2000 drivers of
Ingenic's GPL bare-metal library [libbare-cpu](https://gitee.com/ingenic-dev/libbare-cpu)
with the panel settings read out of the stock image (480x272 RGB panel, 60 Hz; reset on
GPIO PB16, power enable on PC21; PWM backlight).

Build: `tools/rtos-build/build.sh` (needs Docker) gives `tools/rtos-build/out/nocry-rtos.bin`.

If an image hangs, the board stops before Linux. Recover through USB boot mode: switch the
active slot back or write the firmware's own rtos image. The image keeps a text log in RAM
at physical 0x06e00000 (starts with `NCRTLOG1`); after BOOT+RESET, `nocry fastboot` and
`fastboot fetch ram ram.bin` retrieve it.
