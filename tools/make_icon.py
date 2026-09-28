# Draws the Afterglow app icon (1024x1024 PNG) with only the Python standard library.
# Usage: python3 tools/make_icon.py icon.png   (see README, "How the build is set up")
import math, struct, zlib, sys
N = 1024
def clamp(x): return 0.0 if x < 0 else 1.0 if x > 1 else x
def smooth(e0, e1, x):
    t = clamp((x - e0) / (e1 - e0)); return t * t * (3 - 2 * t)
bars = [0.55 + 0.45 * abs(math.sin(i * 1.7) * math.cos(i * 0.61)) for i in range(64)]
rows = []
for y in range(N):
    row = bytearray([0])
    for x in range(N):
        u = (x + 0.5) / N * 2 - 1; v = (y + 0.5) / N * 2 - 1
        # macOS-style rounded square mask (inset).
        m = 0.8; qx = max(abs(u) - m + 0.18, 0); qy = max(abs(v) - m + 0.18, 0)
        d = math.hypot(qx, qy) - 0.18
        a = 1 - smooth(-0.004, 0.004, d)
        if a <= 0: row += b"\0\0\0\0"; continue
        r = math.hypot(u, v); ang = math.atan2(v, u)
        # Background: deep night gradient.
        R = 0.05 + 0.05 * (1 - v) * 0.5; G = 0.03; B = 0.10 + 0.08 * (1 - v) * 0.5
        # Warm glow (orange core to pink rim).
        glow = math.exp(-(r / 0.42) ** 2)
        R += glow * 1.0; G += glow * 0.45; B += glow * 0.35
        # Radial spectrum bars around the ring.
        idx = int(((ang + math.pi) / (2 * math.pi)) * 64) % 64
        frac = ((ang + math.pi) / (2 * math.pi)) * 64 % 1
        inner, outer = 0.47, 0.47 + 0.22 * bars[idx]
        if 0.18 < frac < 0.82 and inner < r < outer:
            t = (r - inner) / (outer - inner)
            R += 1.0 * (1 - t) + 1.0 * t; G += 0.55 * (1 - t) + 0.25 * t; B += 0.2 * (1 - t) + 0.75 * t
        # Bright ring.
        ring = math.exp(-((r - 0.44) / 0.012) ** 2)
        R += ring; G += ring * 0.85; B += ring * 0.8
        row += bytes([int(clamp(R) * 255), int(clamp(G) * 255), int(clamp(B) * 255), int(a * 255)])
    rows.append(bytes(row))
raw = b"".join(rows)
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
