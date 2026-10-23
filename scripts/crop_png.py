"""crop_png.py — 纯标准库 PNG 裁剪（无 PIL 环境）。

用途：把截图裁成局部再看/再分析，避免整图缩放后估坐标失准。
支持 RGBA/RGB 8bit 输入（我们只处理 screencap 产物）。

用法：
  python build/crop_png.py 输入.png 输出.png x0 y0 x1 y1
"""
import struct
import sys
import zlib


def load_png(path):
    data = open(path, "rb").read()
    pos, idat, w = 8, b"", None
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln
    bpp = 3 if ctype == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(w * h * bpp)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bpp, bytes(out)


def write_png(path, w, h, bpp, px):
    raw = bytearray()
    stride = w * bpp
    for y in range(h):
        raw.append(0)
        raw += px[y * stride:(y + 1) * stride]

    def chunk(typ, body):
        return (struct.pack(">I", len(body)) + typ + body
                + struct.pack(">I", zlib.crc32(typ + body) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6 if bpp == 4 else 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
    w, h, bpp, px = load_png(src)
    x0, y0 = max(0, x0), max(0, y0)
    x1, y1 = min(w, x1), min(h, y1)
    cw, ch = x1 - x0, y1 - y0
    out = bytearray(cw * ch * bpp)
    for y in range(ch):
        s = ((y + y0) * w + x0) * bpp
        out[y * cw * bpp:(y + 1) * cw * bpp] = px[s:s + cw * bpp]
    write_png(dst, cw, ch, bpp, bytes(out))
    print("裁剪 %s -> %s  区域 (%d,%d)-(%d,%d)  %dx%d" % (src, dst, x0, y0, x1, y1, cw, ch))


if __name__ == "__main__":
    main()
