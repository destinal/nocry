# nocryboot second stage (work in progress)

The first stage (the SPL, 24 KiB, nearly full) cannot hold what the next features need: reading
the touch chip, looking for a USB flash disk, reading a file from the data partition. A second
stage that the first one loads from the boot area can. See `docs/nocry-rescue.md` for what it
is for: getting into the rescue system by holding the screen, by a one-time flag, or by a flash
disk with a marker file.

## State

`0001-nb2test-experiment.patch` applies on top of `tools/burner-build/patches/k1deb/*` (U-Boot
k1-debian @271fbe7). It adds the target `nocry_nb2test_x2000_v12`: nocry's fastboot stage plus
USB host (the tree's dwc2 driver), USB storage, FAT, ext4, and a command `nb2test` that runs
once at start and prints what it sees:

- the NS2009 touch chip, bit-banged on PC25/PC26 (twenty readings, half a second apart);
- USB host with the two port supplies on (PC9, PC10): what is attached, how long it took,
  whether `nocry-rescue.txt` can be read from a FAT flash disk;
- whether the data partition's ext4 can be listed.

It then goes to fastboot, where `nocry boot-log` reads the output. Nothing is written to the
board. It builds (336 kB). It has not run on a board yet.

To try it: wrap `u-boot.bin` as a uImage (`mkimage -A mips -O linux -T kernel -C none
-a 0x80100000 -e 0x80100000`) and start it through the fastboot stage from memory:
`nocry recovery --image nb2test.uImage` (the command then waits in vain for a recovery system;
that is expected), then `nocry boot-log`.

The image is larger than what the boot ROM's own stage-2 path has been seen to start (about
0x44000 bytes), which is why it is started through the fastboot stage. Whether that limit
applies to a stage loaded from the eMMC by the SPL is not known yet.
