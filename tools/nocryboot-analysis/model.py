# How the hardware answers when the SPL polls it.  Loaded by splemu.py.
# Everything not listed reads back what was written (0 before the first write).
for u in range(5):
    MODEL[0xb0030014 + u * 0x1000] = lambda v: 0x60 | (1 if conin else 0)    # UART: transmitter empty; data ready if input waits
MODEL[0xb00000d4] = lambda v: 0xf0000000                       # clock dividers: change done, all stable
def _ost_lo(v):
    ticks[0] += 240; return ticks[0] & 0xffffffff              # system timer: moves 10 us per read
MODEL[0xb2000010] = _ost_lo
MODEL[0xb2000014] = lambda v: ticks[0] >> 32
MODEL[0xb3012004] = lambda v: v | 1                            # memory PHY PLL locked
MODEL[0xb34f0018] = lambda v: v & ~1                           # memory controller: command done
MODEL[0xb30110c8] = lambda v: v | 8                            # memory PHY: PLL locked
MODEL[0xb30110cc] = lambda v: 3                                # memory PHY: calibration done (16-bit bus: group A)
MODEL[0xb2200020] = lambda v: 2 if core1['started'] else 0     # core 1 is asleep again
for _r in (0xb0000010, 0xb0000014, 0xb0000018):
    MODEL[_r] = lambda v: v | 8 if v & 1 else v                # PLL: "on" follows "enable"
# eMMC host: the routines around it are replaced, but a sector write polls these itself
MODEL[0xb3450030] = lambda v: 0x33            # command done, transfer done, buffer ready
# Touch chip (NS2009) behind the bit-banged I2C on port C: the SPL samples the data pin (bit 26
# of the pin register) nine times per byte, five bytes per reading: three it sends (the chip
# acknowledges by holding the line low) and two it reads (the pressure, 12 bits, high bits first).
_touch_reads = [0]
def _gpc_pin(v):
    i = _touch_reads[0] % 45; _touch_reads[0] += 1
    hi, lo = args.touch >> 4, (args.touch & 0xf) << 4
    bit = (hi >> (34 - i)) & 1 if 27 <= i < 35 else (lo >> (43 - i)) & 1 if 36 <= i < 44 else 0
    return bit << 26
MODEL[0xb0010200] = _gpc_pin
