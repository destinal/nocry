# nocry-rtos

An open replacement for the image in a Nebula board's `rtos` partition (Creality's is a
closed FreeRTOS program that shows the boot logo). Background: `docs/rtos-partition.md`.

Status: **stage 1 works on a Nebula Pad** (2026-11-26, slot B, firmware 1.1.0.30): the
loader accepts the image, gets its answer (`lcm_id=0` on the kernel command line) and Linux
starts with both CPU cores. The image shows no picture, and the **screen stays dark even
after Linux is up**: Creality's Linux loads its display driver with `lcd_is_inited`, so it
relies on the rtos image to have powered and configured the panel and backlight. Bringing
up the display is therefore required, not optional.

Planned next: display and backlight setup and a boot picture, using the X2000 drivers of
Ingenic's GPL bare-metal library [libbare-cpu](https://gitee.com/ingenic-dev/libbare-cpu)
with the panel settings read out of the stock image (480x272 RGB panel, 60 Hz; reset on
GPIO PB16, power enable on PC21; PWM backlight).

Build: `tools/rtos-build/build.sh` (needs Docker) gives `tools/rtos-build/out/nocry-rtos.bin`.

If an image hangs, the board stops before Linux. Recover through USB boot mode: switch the
active slot back or write the firmware's own rtos image. The image keeps a text log in RAM
at physical 0x06e00000 (starts with `NCRTLOG1`); after BOOT+RESET, `nocry fastboot` and
`fastboot fetch ram ram.bin` retrieve it.
