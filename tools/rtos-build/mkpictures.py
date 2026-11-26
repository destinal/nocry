#!/usr/bin/env python3
"""Packs boot pictures into the store nocry-rtos reads (format: rtos/pictures.c).

  mkpictures.py -o pictures.bin [--default FILE] [MODEL=FILE ...] [--from-stock ZERO.BIN]

MODEL is the model field of the board's sn_mac record ("Nebula Pad", "F005", ...).
--from-stock takes the boot logo out of a firmware's own rtos image (zero.bin) for the
models that image knows, so a board keeps its original look without that artwork being
part of nocry. Needs Pillow.
"""
import argparse, io, re, struct, sys
from PIL import Image

PANEL = (480, 272)

def rle565(img):
    img = img.convert('RGB')
    w, h = img.size
    px = img.load()
    out = bytearray()
    run, cur = 0, None
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            v = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)
            if v == cur and run < 0xffff:
                run += 1
            else:
                if run:
                    out += struct.pack('<HH', run, cur)
                run, cur = 1, v
    out += struct.pack('<HH', run, cur)
    return w, h, bytes(out)

def stock_pictures(path):
    """(model, image) pairs from a stock rtos image: the logo made for this panel."""
    d = open(path, 'rb').read()
    found = []
    for m in re.finditer(b'\xff\xd8\xff', d):
        try:                                      # the decoder stops at the picture's own end marker
            im = Image.open(io.BytesIO(d[m.start():m.start() + (4 << 20)])); im.load()
        except Exception:
            continue
        found.append(im)
    fit = [im for im in found if im.size == PANEL]
    if not fit:
        sys.exit('%s: no %dx%d picture inside' % (path, *PANEL))
    models = [m.decode() for m in re.findall(rb'(Nebula Pad|F00\d)\0', d)]
    return [(m, fit[0]) for m in dict.fromkeys(models)]

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--default')
    ap.add_argument('--from-stock')
    ap.add_argument('pictures', nargs='*', metavar='MODEL=FILE')
    a = ap.parse_args()
    pics = {}
    if a.from_stock:
        for m, im in stock_pictures(a.from_stock):
            pics[m] = im
    for p in a.pictures:
        m, f = p.split('=', 1)
        pics[m] = Image.open(f)
    if a.default:
        pics[''] = Image.open(a.default)
    if not pics:
        sys.exit('no pictures given')
    entries, data = [], bytearray()
    head = 8 + 48 * len(pics)
    cache = {}
    for m, im in pics.items():
        if len(m.encode()) > 31:
            sys.exit('model name too long: ' + m)
        key = im.tobytes() + bytes(str(im.size), 'ascii')
        if key not in cache:                      # identical pictures are stored once
            w, h, r = rle565(im)
            cache[key] = (w, h, head + len(data), len(r))
            data += r
        w, h, off, ln = cache[key]
        entries.append(struct.pack('<32sIIII', m.encode(), w, h, off, ln))
        print('  %-12s %dx%d, %d bytes' % (repr(m) if m else '(default)', w, h, ln))
    blob = b'NCPX' + struct.pack('<I', len(entries)) + b''.join(entries) + data
    open(a.out, 'wb').write(blob)
    print('%s: %d pictures, %d bytes' % (a.out, len(entries), len(blob)))

if __name__ == '__main__':
    main()
