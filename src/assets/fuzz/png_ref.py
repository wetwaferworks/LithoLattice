# Differential reference for lk_png_check (decode mode): the same w, h and FNV-1a64 of RGBA,
# Run: ls *.png | python3 png_ref.py > ref.txt; lk_png_check decode *.png > ours.txt; diff ref.txt ours.txt (paths on stdin).
# Independent reference: Python zlib + straightforward unfilter, same FNV-1a over RGBA.
import sys, zlib, struct
def fnv(b):
    h = 1469598103934665603
    for c in b: h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h
def dec(path):
    d = open(path, 'rb').read(); p = 8; z = b''; pal = None; trns = None
    while p < len(d):
        n, t = struct.unpack('>I4s', d[p:p+8]); body = d[p+8:p+8+n]; p += 12 + n
        if t == b'IHDR': w, h, bd, ct, _, _, il = struct.unpack('>IIBBBBB', body)
        elif t == b'PLTE': pal = body
        elif t == b'tRNS': trns = body
        elif t == b'IDAT': z += body
        elif t == b'IEND': break
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]; raw = zlib.decompress(z); st = w * ch
    out = bytearray(); prev = bytearray(st)
    for y in range(h):
        f = raw[y*(st+1)]; cur = bytearray(raw[y*(st+1)+1:(y+1)*(st+1)])
        for i in range(st):
            a = cur[i-ch] if i >= ch else 0; b = prev[i]; c = prev[i-ch] if i >= ch else 0
            if f == 1: cur[i] = (cur[i] + a) & 255
            elif f == 2: cur[i] = (cur[i] + b) & 255
            elif f == 3: cur[i] = (cur[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                pp = a + b - c; pa, pb, pc = abs(pp-a), abs(pp-b), abs(pp-c)
                cur[i] = (cur[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        for x in range(w):
            if ct == 6: out += cur[4*x:4*x+4]
            elif ct == 2:
                r, g, bb = cur[3*x:3*x+3]; k = trns and struct.unpack('>HHH', trns) == (r, g, bb)
                out += bytes((r, g, bb, 0 if k else 255))
            elif ct == 0:
                v = cur[x]; k = trns and struct.unpack('>H', trns)[0] == v; out += bytes((v, v, v, 0 if k else 255))
            elif ct == 4: out += bytes((cur[2*x],)*3 + (cur[2*x+1],))
            else:
                i = cur[x]; out += pal[3*i:3*i+3] + bytes((trns[i] if trns and i < len(trns) else 255,))
        prev = cur
    return w, h, fnv(out)
for line in sys.stdin:
    pth = line.strip(); w, h, f = dec(pth); print(f"{pth} {w} {h} {f:016x}")
