"""find_button.py — 在截图里找纯色按钮（不依赖 PIL）。

为什么要它：ViewImage 看到的是缩放图，凭视觉估的坐标换算回 2608x1200 会有
几十像素偏差；ghost_tap 打到按钮边缘外就没反应，看起来像"注入没生效"。
这里直接按像素找色块，给的是真实屏幕坐标。

用法：
  python build/find_button.py build/hcr_practice.png green
  python build/find_button.py build/hcr_practice.png red
"""
import struct
import sys
import zlib


def load_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not png"
    pos = 8
    width = height = None
    idat = b""
    bitdepth = colortype = None
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            width, height, bitdepth, colortype = struct.unpack(">IIBB", body[:10])
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln
    assert bitdepth == 8 and colortype in (2, 6), \
        "unsupported png: depth=%s colortype=%s" % (bitdepth, colortype)
    bpp = 3 if colortype == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * bpp
    out = bytearray(width * height * bpp)
    prev = bytearray(stride)
    p = 0
    for y in range(height):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ft == 1:      # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:    # Average
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:    # Paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif ft != 0:
            raise SystemExit("bad filter %d at row %d" % (ft, y))
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, bpp, bytes(out)


def is_green(r, g, b):
    return g > 120 and g > r + 50 and g > b + 50


def is_red(r, g, b):
    return r > 120 and r > g + 50 and r > b + 50


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "build/hcr_practice.png"
    want = sys.argv[2] if len(sys.argv) > 2 else "green"
    test = is_green if want == "green" else is_red
    w, h, bpp, px = load_png(path)

    # 按 32px 网格统计，找连续色块所在的格子，再在格子内求精确包围盒
    CELL = 32
    best = []
    for cy in range(0, h, CELL):
        for cx in range(0, w, CELL):
            cnt = 0
            for y in range(cy, min(cy + CELL, h), 4):
                row = y * w * bpp
                for x in range(cx, min(cx + CELL, w), 4):
                    o = row + x * bpp
                    if test(px[o], px[o + 1], px[o + 2]):
                        cnt += 1
            if cnt > 120:      # 该格 ~64 个采样点里有 60% 以上是目标色
                best.append((cnt, cx, cy))
    best.sort(reverse=True)
    if not best:
        print("没找到 %s 色块" % want)
        return 1
    print("图像 %dx%d，命中格子（按密度排序，取前 6）：" % (w, h))
    for cnt, cx, cy in best[:6]:
        # 在格子内求包围盒
        minx, miny, maxx, maxy = 10 ** 9, 10 ** 9, -1, -1
        for y in range(cy, min(cy + CELL, h)):
            row = y * w * bpp
            for x in range(cx, min(cx + CELL, w)):
                o = row + x * bpp
                if test(px[o], px[o + 1], px[o + 2]):
                    minx, maxx = min(minx, x), max(maxx, x)
                    miny, maxy = min(miny, y), max(maxy, y)
        print("  cell(%d,%d) 采样点 %d  局部bbox x=%d..%d y=%d..%d  中心≈(%d,%d)"
              % (cx, cy, cnt, minx, maxx, miny, maxy,
                 (minx + maxx) // 2, (miny + maxy) // 2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
