#!/usr/bin/env python3
"""shot.py USB_LOG OUT.png [N]: ask the live run for 'shot N', cut the newest framed picture out of the log into a PNG."""
import sys, time, zlib, struct, re, os
log, out = sys.argv[1], sys.argv[2]
n = sys.argv[3] if len(sys.argv) > 3 else "3"
start = os.path.getsize(log)
with open("/dev/cu.usbmodem01", "wb", buffering=0) as p:
    p.write(f"shot {n}\r".encode())
for _ in range(60):
    time.sleep(0.5)
    data = open(log, "rb").read()[start:]
    m = None
    for m in re.finditer(rb"SHOT (\d+) (\d+) (\d+) rgb888 (\d+)\n", data):
        pass
    if m and len(data) >= m.end() + int(m.group(4)):
        break
else:
    sys.exit("no picture")
w, h, stride, size = (int(x) for x in m.groups())
px = data[m.end():m.end() + size]
raw = b"".join(b"\0" + px[y * stride:y * stride + w * 3] for y in range(h))
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
print(out, w, h)
