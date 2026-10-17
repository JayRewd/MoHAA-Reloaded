# Tile screenshots into one image to look at them side by side (no PIL on this machine).
#   python3 -I port/tgamontage.py out.tga <columns> <downscale> in1.tga in2.tga ...
# Uncompressed 24/32-bit TGAs (the game's screenshots, tgadiff's diff images), nearest-neighbor downscale;
# convert the result with `sips -s format png out.tga --out out.png`.
import sys, struct
def rd(p):
    d = open(p, "rb").read()
    idl, cmt, it = d[0], d[1], d[2]
    w, h = struct.unpack("<HH", d[12:16]); bpp = d[16] // 8; desc = d[17]
    assert it == 2, "uncompressed only"
    px = d[18 + idl:]
    top = bool(desc & 0x20)
    rows = [px[(y if top else h - 1 - y) * w * bpp:((y if top else h - 1 - y) + 1) * w * bpp] for y in range(h)]
    return w, h, bpp, rows
out, cols, sc = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
ims = [rd(p) for p in sys.argv[4:]]
w, h = ims[0][0] // sc, ims[0][1] // sc
n = len(ims); rows_n = (n + cols - 1) // cols
W, H = w * cols, h * rows_n
buf = bytearray(W * H * 3)
for i, (iw, ih, bpp, rows) in enumerate(ims):
    ox, oy = (i % cols) * w, (i // cols) * h
    for y in range(h):
        r = rows[y * sc]
        o = ((oy + y) * W + ox) * 3
        for x in range(w):
            s = x * sc * bpp
            buf[o + x * 3:o + x * 3 + 3] = r[s:s + 3]
hdr = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, W, H, 24, 0x20)
open(out, "wb").write(hdr + bytes(buf))
