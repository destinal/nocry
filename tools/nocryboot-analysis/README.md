# nocryboot analysis

`splemu.py` runs a first-stage loader (SPL) of an X2000E board in the Unicorn emulator, from its first instruction to the jump into the kernel, and logs every hardware register access, the console output, the eMMC reads and the kernel arguments. `model.py` holds the few answers the hardware gives when polled (PLL locked, memory training done, ...).

`compare.sh <nebula|k1> <backup image>` runs the factory SPL from a backup image and nocryboot's build, and prints the differences. Expected output is listed in `docs/nocryboot.md`.

Limits:

- The eMMC is not emulated at register level. The routines "send a command" and "read blocks" are replaced; reads come from the image file. Commands are answered like an eMMC (no answer to the SD-only ones).
- The rtos image is not run. When the SPL starts CPU core 1, the emulator changes the image's first word and stores a panel id (`--lcm N`, `--lcm -1` for none), which is what a finished image does.
- Registers read back what was written; reset values are 0. A read-modify-write of a reset value is therefore invisible here.
- `--slot a|b` overrides the `ota` record of the image; `--blank rtos,kernel2,...` reads partitions as zeros; `--copy-a-to-b` gives slot B the content of slot A; `--console-input '\x1b\x1b\x1b'` puts characters at the serial console. `--note TEXT` puts a text into the second sector of `ota`; `--emmc FILE` keeps what the SPL writes between runs, so several runs form a sequence of starts. The run ends at the jump into the kernel or at a restart into USB boot mode.

Needs: python3 with `unicorn`, `nm` (binutils).
