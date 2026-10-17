#!/usr/bin/env python3
# tgadiff.py a.tga b.tga [diff.tga]: per-pixel RGB difference of two game screenshots (uncompressed or RLE TGA)
import sys

def read_tga(path):
    d = open(path, 'rb').read()
    idlen, cmtype, itype = d[0], d[1], d[2]
    w, h = d[12] | d[13] << 8, d[14] | d[15] << 8
    bpp, desc = d[16], d[17]
    bp = bpp // 8
    p = 18 + idlen
    n = w * h
    if itype == 2:
        px = d[p:p + n * bp]
    elif itype == 10:
        out = bytearray()
        while len(out) < n * bp:
            c = d[p]; p += 1
            k = (c & 0x7f) + 1
            if c & 0x80:
                out += d[p:p + bp] * k; p += bp
            else:
                out += d[p:p + k * bp]; p += k * bp
        px = bytes(out)
    else:
        raise SystemExit('unsupported TGA type %d' % itype)
    rows = [px[y * w * bp:(y + 1) * w * bp] for y in range(h)]
    if not (desc & 0x20):
        rows.reverse()  # bottom-up -> top-down
    return w, h, bp, rows

wa, ha, ba, ra = read_tga(sys.argv[1])
wb, hb, bb, rb = read_tga(sys.argv[2])
if (wa, ha) != (wb, hb):
    raise SystemExit('size differs: %dx%d vs %dx%d' % (wa, ha, wb, hb))
hist = [0] * 256
total = 0
out = bytearray()
worst = (0, 0, 0)
for y in range(ha):
    a, b = ra[y], rb[y]
    for x in range(wa):
        m = 0
        for c in range(3):
            v = abs(a[x * ba + c] - b[x * bb + c])
            total += v
            if v > m:
                m = v
        hist[m] += 1
        if m > worst[0]:
            worst = (m, x, y)
        out += bytes((min(255, m * 16),) * 3)
n = wa * ha
print('%dx%d: mean %.4f/channel, max %d at (%d,%d) top-left; pixels differing: >0 %d (%.2f%%), >2 %d, >8 %d, >32 %d' % (
    wa, ha, total / (3.0 * n), worst[0], worst[1], worst[2],
    n - hist[0], 100.0 * (n - hist[0]) / n, sum(hist[3:]), sum(hist[9:]), sum(hist[33:])))
if len(sys.argv) > 3:
    hdr = bytes([0, 0, 2] + [0] * 9 + [wa & 255, wa >> 8, ha & 255, ha >> 8, 24, 0x20])
    open(sys.argv[3], 'wb').write(hdr + bytes(out))
