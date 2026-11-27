/*
 * nocry-touch: is the Nebula Pad's screen being pressed?  SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Reads the pressure from the NS2009 touch chip (I2C address 0x48) through i2c-dev.
 *   nocry-touch [--bus N]            print the pressure (0 = not touched, about 2200 = a finger)
 *   nocry-touch [--bus N] --held     exit 0 if pressed, 1 if not, 2 if the chip does not answer
 * The same threshold as the nocryboot loader uses (600 of 4095).
 */
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int pressure(int fd)
{
	unsigned char cmd = 0xe0, b[2];
	if (write(fd, &cmd, 1) != 1) return -1;
	usleep(200);
	if (read(fd, b, 2) != 2) return -1;
	return b[0] << 4 | b[1] >> 4;
}

int main(int argc, char **argv)
{
	int bus = 4, held = 0, i, fd, z; char dev[32];
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--bus") && i + 1 < argc) bus = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--held")) held = 1;
		else { fprintf(stderr, "usage: nocry-touch [--bus N] [--held]\n"); return 2; }
	}
	snprintf(dev, sizeof dev, "/dev/i2c-%d", bus);
	fd = open(dev, O_RDWR);
	if (fd < 0 || ioctl(fd, I2C_SLAVE, 0x48) < 0) { if (!held) perror(dev); return 2; }
	z = pressure(fd);
	if (z < 0) { if (!held) fprintf(stderr, "the touch chip does not answer\n"); return 2; }
	if (held) return z >= 600 ? 0 : 1;
	printf("%d\n", z);
	return 0;
}
