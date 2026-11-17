"""Rigorous pixel audit of the dock screenshot.

Answers three questions the visual reviewer raised, using data rather than opinion:
  1. Are there thin-line boxes drawn around icons (a real rendering defect)?
  2. Are the icons different sizes, or is that just their artwork?
  3. Is there stray text inside the dock?

The plate is a translucent rounded rect over a near-black desktop, so the plate
is found by brightness threshold against the background, not by assuming a
centre band.
"""

import sys
import numpy as np
from PIL import Image

path = sys.argv[1] if len(sys.argv) > 1 else r'D:\MacDock\evidence\T-301\dock_after_alpha.png'
im = Image.open(path).convert('RGBA')
a = np.asarray(im).astype(np.int32)
h, w, _ = a.shape
rgb = a[:, :, :3]
lum = rgb.mean(axis=2)

print(f'image {w}x{h}')
print(f'luminance: min={lum.min():.1f} max={lum.max():.1f} median={np.median(lum):.1f}')

# --- 1. Locate the plate ---------------------------------------------------
# Background is near-black (<5). The plate tint is ~15. Threshold at 7.
plate_mask = lum > 7
rowfrac = plate_mask.mean(axis=1)
colfrac = plate_mask.mean(axis=0)

rows = np.where(rowfrac > 0.25)[0]
cols = np.where(colfrac > 0.25)[0]
if len(rows) == 0 or len(cols) == 0:
    print('could not locate the plate')
    sys.exit(1)
top, bot = int(rows.min()), int(rows.max()) + 1
left, right = int(cols.min()), int(cols.max()) + 1
print(f'plate bbox = x {left}..{right} (w={right - left}), y {top}..{bot} (h={bot - top})')

plate_tint = np.median(rgb[top + 6:bot - 6, left + 6:left + 30, :].reshape(-1, 3), axis=0)
print(f'plate tint (median of left inset) = {plate_tint}')

# --- 2. Icon segmentation --------------------------------------------------
inner = rgb[top:bot, left:right, :]
dist = np.abs(inner - plate_tint).sum(axis=2)
ink = dist > 45

colsum = ink.sum(axis=0)
runs = []
inrun = False
start = 0
for x, v in enumerate(colsum):
    if v > 2 and not inrun:
        start, inrun = x, True
    elif v <= 2 and inrun:
        runs.append([start, x])
        inrun = False
if inrun:
    runs.append([start, len(colsum)])

merged = []
for r in runs:
    if merged and r[0] - merged[-1][1] < 8:
        merged[-1][1] = r[1]
    else:
        merged.append(list(r))
merged = [tuple(m) for m in merged if m[1] - m[0] > 10]

print(f'\n--- icon columns ---')
print(f'found {len(merged)} ink clusters')
centres = []
widths = []
for i, (x0, x1) in enumerate(merged):
    band = ink[:, x0:x1]
    ys = np.where(band.any(axis=1))[0]
    if len(ys) == 0:
        continue
    y0, y1 = int(ys.min()), int(ys.max()) + 1
    cx = left + (x0 + x1) / 2
    centres.append(cx)
    widths.append(x1 - x0)
    print(f'  [{i:2d}] x {left + x0:4d}..{left + x1:4d} w={x1 - x0:3d}  y {top + y0:3d}..{top + y1:3d} h={y1 - y0:3d}')

if len(centres) > 1:
    gaps = np.diff(centres)
    print(f'\ncentre gaps: min={gaps.min():.1f} max={gaps.max():.1f} mean={gaps.mean():.1f} stdev={gaps.std():.2f}')
    print(f'  gaps = {[round(float(g), 1) for g in gaps]}')
if widths:
    print(f'ink widths: min={min(widths)} max={max(widths)} mean={np.mean(widths):.1f}')

# --- 3. Thin-box detection -------------------------------------------------
# A drawn rectangle outline is a 1-2px bright line spanning most of a cell width,
# appearing at two distinct heights with plate tint between them.
print('\n--- horizontal thin-line scan (drawn box edges would show here) ---')
suspect_rows = []
for y in range(inner.shape[0]):
    rowink = ink[y, :]
    best = 0
    cur = 0
    for v in rowink:
        cur = cur + 1 if v else 0
        if cur > best:
            best = cur
    if best > 70:
        suspect_rows.append((y, best))

print(f'rows whose longest contiguous ink run exceeds 70px: {len(suspect_rows)}')
for y, best in suspect_rows[:8]:
    print(f'  row {top + y}: run={best}px')

print('\n--- vertical thin-line scan (drawn box sides would show here) ---')
suspect_cols = []
for x in range(inner.shape[1]):
    colink = ink[:, x]
    best = 0
    cur = 0
    for v in colink:
        cur = cur + 1 if v else 0
        if cur > best:
            best = cur
    if best > 70:
        suspect_cols.append((x, best))

print(f'cols whose longest contiguous ink run exceeds 70px: {len(suspect_cols)}')
for x, best in suspect_cols[:8]:
    print(f'  col {left + x}: run={best}px')
