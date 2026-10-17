#!/usr/bin/env python3
# A RoQ test video for the cinematic path (RE_StretchRaw / RE_UploadCinematic): retail MOHAA data ships no
# video, and there is no RoQ encoder on this machine. Writes 256x256 pictures of 1024 8x8 blocks, each a 4x4
# codebook cell scaled 2x (made of four 2x2 cells with a luma ramp inside, so the image has 1-pixel detail that
# shows how the renderer stretches it), at 30 fps: a different picture every frame for the first 1.5 s (each one
# a new upload of a texture the frames in flight still use), then a final picture held until <seconds>. Captures
# taken after 2 s show the final picture in every run.
#   port/roqtest.py <out.roq> [seconds]
# Play it with `cinematic <name>.roq` (the file goes into <fs_homepath>/main/video/). id's decoder (cl_cin.cpp):
# chunks are [id u16][size u32][arg u16][data]; VQ code words hold eight 2-bit codes, read from the top bits and
# fetched right before the arguments of their first code. Code 2 (SLD) takes one byte: a 4x4 cell for an 8x8 block.
import struct
import sys

ROQ_SIGNATURE = 0x1084
ROQ_QUAD_INFO = 0x1001
ROQ_CODEBOOK = 0x1002
ROQ_QUAD_VQ = 0x1011
SIZE = 256
FPS = 30


def chunk(cid, data, arg=0):
    return struct.pack('<HIH', cid, len(data), arg) + data


def main():
    out = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0

    # 256 2x2 cells (y0 y1 y2 y3 u v) and 256 4x4 cells (four 2x2 indices); arg 0 = 256 of each
    book = bytearray()
    for k in range(256):
        y = 30 + (k * 37) % 160
        book += bytes([y, y + 20, y + 40, y + 60, (k * 53) % 256, (k * 97) % 256])
    for j in range(256):
        book += bytes([j, (j + 1) % 256, (j + 16) % 256, (j + 17) % 256])

    # a picture: every 8x8 block (1024 of them, in the decoder's quad order) is code 2 with its cell
    def picture(k):
        frame = bytearray()
        blocks = [(b * 7 + b // 32 + k * 13) % 256 for b in range(SIZE * SIZE // 64)]
        for i in range(0, len(blocks), 8):
            frame += struct.pack('<H', 0xaaaa) + bytes(blocks[i:i + 8])
        return bytes(frame)
    # every block skipped (code 0): the picture stays. id's decoder keeps two buffers and alternates between them,
    # so the final picture is written twice before the skipped frames.
    changing = FPS * 3 // 2
    still = bytes(2 * SIZE * SIZE // 64 // 8)

    data = struct.pack('<HIH', ROQ_SIGNATURE, 0xffffffff, FPS)
    data += chunk(ROQ_QUAD_INFO, struct.pack('<HHHH', SIZE, SIZE, 8, 4))
    data += chunk(ROQ_CODEBOOK, bytes(book))
    for k in range(int(seconds * FPS)):
        data += chunk(ROQ_QUAD_VQ, picture(min(k, changing)) if k <= changing + 1 else still)
    with open(out, 'wb') as f:
        f.write(data)
    print('%s: %d bytes, %d frames' % (out, len(data), int(seconds * FPS)))


if __name__ == '__main__':
    main()
