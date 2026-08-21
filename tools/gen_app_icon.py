#!/usr/bin/env python3
"""Generate res/icons/tray_icon.ico (multi-size, premultiplied box-downsampled)
from res/icons/tray_icon.png (expected 8-bit RGBA).

Pure stdlib (zlib only). Keeps the EXE/app icon consistent with the tray icon.

Run: python tools/gen_app_icon.py
"""
import struct
import zlib
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "res", "icons", "tray_icon.png"))
DST = os.path.normpath(os.path.join(HERE, "..", "res", "icons", "tray_icon.ico"))
SIZES = [256, 128, 64, 48, 32, 16]


def load_rgba(path):
    """Decode an 8-bit RGBA PNG to a top-down RGBA bytearray."""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    w = struct.unpack(">I", data[16:20])[0]
    h = struct.unpack(">I", data[20:24])[0]
    bitd = data[24]
    ctype = data[25]
    assert (bitd, ctype) == (8, 6), "need 8-bit RGBA, got bitd=%d ctype=%d" % (bitd, ctype)

    pos = 8
    idat = b""
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        if typ == b"IDAT":
            idat += data[pos + 8:pos + 8 + ln]
        elif typ == b"IEND":
            break
        pos += 12 + ln

    raw = zlib.decompress(idat)
    stride = w * 4
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ftype = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ftype == 1:
            for i in range(4, stride):
                line[i] = (line[i] + line[i - 4]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                a = line[i - 4] if i >= 4 else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                a = line[i - 4] if i >= 4 else 0
                b = prev[i]
                c = prev[i - 4] if i >= 4 else 0
                pp = a + b - c
                pa = abs(pp - a)
                pb = abs(pp - b)
                pc = abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, out


def downscale_premul(src, sw, sh, dw, dh):
    """Premultiplied-alpha box downsample (separable 2-pass) -> top-down RGBA."""
    tmpR = [0.0] * (dw * sh)
    tmpG = [0.0] * (dw * sh)
    tmpB = [0.0] * (dw * sh)
    tmpA = [0.0] * (dw * sh)
    tmpC = [0] * (dw * sh)
    # pass 1: horizontal
    for y in range(sh):
        rb = y * sw * 4
        for ox in range(dw):
            x0 = int(ox * sw / dw)
            x1 = max(x0 + 1, int((ox + 1) * sw / dw))
            ar = ag = ab = aa = 0
            cnt = 0
            for xx in range(x0, x1):
                o = rb + xx * 4
                A = src[o + 3]
                ar += src[o] * A
                ag += src[o + 1] * A
                ab += src[o + 2] * A
                aa += A
                cnt += 1
            d = y * dw + ox
            tmpR[d] = ar
            tmpG[d] = ag
            tmpB[d] = ab
            tmpA[d] = aa
            tmpC[d] = cnt
    # pass 2: vertical
    dst = bytearray(dw * dh * 4)
    for x in range(dw):
        for oy in range(dh):
            y0 = int(oy * sh / dh)
            y1 = max(y0 + 1, int((oy + 1) * sh / dh))
            ar = ag = ab = aa = cc = 0
            for yy in range(y0, y1):
                d = yy * dw + x
                ar += tmpR[d]
                ag += tmpG[d]
                ab += tmpB[d]
                aa += tmpA[d]
                cc += tmpC[d]
            inv = 1.0 / aa if aa > 0 else 0.0
            o = (oy * dw + x) * 4
            dst[o] = int(ar * inv) & 0xFF
            dst[o + 1] = int(ag * inv) & 0xFF
            dst[o + 2] = int(ab * inv) & 0xFF
            dst[o + 3] = int(aa / cc) & 0xFF if cc > 0 else 0
    return dst


def encode_ico(images):
    """images: list of (size, top-down RGBA bytes). Returns .ico bytes."""
    entries = []
    bodys = []
    offset = 6 + 16 * len(images)
    for size, px in images:
        s = size
        xor = bytearray(s * s * 4)
        for y in range(s):
            sr = y * s * 4
            dr = (s - 1 - y) * s * 4  # bottom-up
            for x in range(s):
                o = sr + x * 4
                d = dr + x * 4
                xor[d] = px[o + 2]      # B
                xor[d + 1] = px[o + 1]  # G
                xor[d + 2] = px[o]      # R
                xor[d + 3] = px[o + 3]  # A
        and_row = ((s + 31) // 32) * 4
        and_mask = bytearray(and_row * s)
        bmp = struct.pack("<IiiHHIIiiII", 40, s, 2 * s, 1, 32, 0, 0, 0, 0, 0, 0)
        bmp += xor + and_mask
        entries.append((s, len(bmp), offset))
        bodys.append(bmp)
        offset += len(bmp)
    icondir = b"\x00\x00\x01\x00" + struct.pack("<H", len(images))
    for s, blen, off in entries:
        b = s & 0xFF
        icondir += bytes([b, b, 0, 0]) + struct.pack("<HH", 1, 32) + struct.pack("<II", blen, off)
    return icondir + b"".join(bodys)


def main():
    sw, sh, px = load_rgba(SRC)
    assert sw == sh, "source expected square, got %dx%d" % (sw, sh)
    images = [(s, downscale_premul(px, sw, sh, s, s)) for s in SIZES if s <= sw]
    ico = encode_ico(images)
    with open(DST, "wb") as f:
        f.write(ico)
    print("wrote", DST, "sizes", [s for s, _ in images], "bytes", len(ico))


if __name__ == "__main__":
    main()
