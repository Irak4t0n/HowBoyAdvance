#!/usr/bin/env python3
"""Generate 16x16, 32x32, and 64x64 HowBoyAdvance launcher icons — no external dependencies."""
import zlib, struct

def make_png_rgba(pixels):
    w, h = len(pixels[0]), len(pixels)
    sig = b'\x89PNG\r\n\x1a\n'
    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xFFFFFFFF)
    ihdr = chunk(b'IHDR', struct.pack('>II', w, h) + bytes([8, 6, 0, 0, 0]))
    raw  = b''.join(b'\x00' + bytes([v for px in row for v in px]) for row in pixels)
    idat = chunk(b'IDAT', zlib.compress(raw, 9))
    iend = chunk(b'IEND', b'')
    return sig + ihdr + idat + iend

# -- Palette ----------------------------------------------------------------
T = (  0,   0,   0,   0)   # transparent
B = ( 60,  50, 100, 255)   # body (indigo/purple — GBA colour)
D = ( 40,  32,  72, 255)   # body darker (edges/grip bumps)
S = ( 15,  15,  20, 255)   # screen bezel (near-black)
G = ( 80, 200, 120, 255)   # screen (bright green — GBA backlit feel)
P = ( 90,  85, 105, 255)   # d-pad grey-purple
A = (200,  30,  30, 255)   # A button (red)
a = (150,  20,  20, 255)   # B button (darker red)
W = (170, 165, 180, 255)   # start / select
L = ( 50,  42,  85, 255)   # shoulder button colour

# -- 32x32 Canvas -----------------------------------------------------------
g = [[T] * 32 for _ in range(32)]

def fill(r0, r1, c0, c1, col):
    for row in range(r0, r1):
        for col_ in range(c0, c1):
            g[row][col_] = col

def dot(r, c, col):
    g[r][c] = col

# GBA is landscape/wide — body fills most of the 32x32 canvas
# Main body (wide rectangle, rows 6-26, cols 1-30)
fill(6, 26, 1, 31, B)

# Shoulder buttons (top edge bumps, rows 4-6)
fill(4, 6, 2, 10, L)    # left shoulder
fill(4, 6, 22, 30, L)   # right shoulder

# Grip bumps on sides (darker strips)
fill(6, 26, 1, 3, D)    # left grip
fill(6, 26, 29, 31, D)  # right grip

# Screen bezel (dark border) — centered, landscape aspect ratio
fill(8, 20, 6, 20, S)

# Screen (green interior)
fill(9, 19, 7, 19, G)

# D-pad (left side, below screen)
# vertical bar
for row in range(21, 26): dot(row, 5, P)
# horizontal bar
for col in range(3, 8): dot(23, col, P)

# A button (right side, upper)
fill(21, 24, 24, 27, A)

# B button (right side, lower-left of A)
fill(23, 26, 21, 24, a)

# Start / Select — two small bars below screen
for c in range(11, 14): dot(21, c, W)   # SELECT
for c in range(15, 18): dot(21, c, W)   # START

# -- Scale helpers -----------------------------------------------------------
def scale_down(grid, factor):
    """Nearest-neighbour downsample: take top-left pixel of each NxN block."""
    return [[grid[r * factor][c * factor]
             for c in range(len(grid[0]) // factor)]
            for r in range(len(grid) // factor)]

def scale_up(grid, factor):
    """Nearest-neighbour upsample: each pixel becomes an NxN block."""
    out = []
    for row in grid:
        expanded = [px for px in row for _ in range(factor)]
        for _ in range(factor):
            out.append(expanded)
    return out

# -- Write all three sizes ---------------------------------------------------
for size, pixels in [
    (16, scale_down(g, 2)),
    (32, g),
    (64, scale_up(g, 2)),
]:
    data = make_png_rgba(pixels)
    fname = f'icon-{size}x{size}.png'
    with open(fname, 'wb') as f:
        f.write(data)
    print(f"{fname} written ({len(data)} bytes)")

# Also keep icon.png (32x32) for the badge /int/apps path
import shutil
shutil.copy('icon-32x32.png', 'icon.png')
print("icon.png (32x32 copy) written")
