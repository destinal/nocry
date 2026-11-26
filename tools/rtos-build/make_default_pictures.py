#!/usr/bin/env python3
"""Draws nocry-rtos' own boot pictures into rtos/pictures/ (run once; the PNGs are committed).

The Nebula Pad's 480x272 panel is used upright (272 wide), so the pictures are drawn upright
and stored turned, like the stock logo."""
import sys
from PIL import Image, ImageDraw, ImageFont

FONT = sys.argv[2] if len(sys.argv) > 2 else '/usr/share/fonts/noto/NotoSans-Bold.ttf'
OUT = sys.argv[1]
BG, FG, DIM, ACCENT = (16, 18, 22), (232, 232, 232), (120, 126, 136), (47, 128, 237)

def picture(sub):
    im = Image.new('RGB', (272, 480), BG)
    d = ImageDraw.Draw(im)
    big, small = ImageFont.truetype(FONT, 54), ImageFont.truetype(FONT, 17)
    w = d.textlength('nocry', font=big)
    d.text(((272 - w) / 2, 190), 'nocry', font=big, fill=FG)
    d.rectangle([(272 - 44) / 2, 266, (272 + 44) / 2, 269], fill=ACCENT)
    if sub:
        w = d.textlength(sub, font=small)
        d.text(((272 - w) / 2, 284), sub, font=small, fill=DIM)
    # few colours, so the run-length data stays small
    return im.rotate(90, expand=True).quantize(colors=16, dither=Image.Dither.NONE).convert('RGB')

for name, sub in (('default', ''), ('nebula-pad', 'Nebula Pad'), ('f003', 'CR-10 SE'), ('f004', 'Ender-5 Max'), ('f005', 'Ender-3 V3 KE')):
    picture(sub).save('%s/%s.png' % (OUT, name), optimize=True)
    print(name)
