"""Decisive test: is a thin stroke rectangle drawn around each dock icon slot?

The dock layout is known analytically: icon_size=48 DIP, gap=8 DIP, scale=2, so
pitch = 112 px and each slot is 96x96 px. If a stroke is drawn around the slot,
there will be a bright column at slot.left and slot.right, and bright rows at
slot.top and slot.bottom, independent of what the icon artwork looks like.

This prints the raw luminance along the boundary columns/rows so the answer is
readable, not inferred.
"""

import sys
import numpy as np
from PIL import Image

path = sys.argv[1]
im = Image.open(path).convert('RGB')
a = np.asarray(im).astype(np.float64)
h, w, _ = a.shape
lum = a.mean(axis=2)

# Locate the plate: opaque region brighter than the near-black desktop.
mask = lum > 7
rows = np.where(mask.mean(axis=1) > 0.25)[0]
plate_top, plate_bot = int(rows.min()), int(rows.max()) + 1
print(f'{path} {w}x{h}')
print(f'plate rows {plate_top}..{plate_bot}')

# The icon baseline is the plate bottom minus the 10 DIP padding (20 px).
baseline = plate_bot - 20
icon_px = 96
slot_top = baseline - icon_px
print(f'derived baseline={baseline} slot_top={slot_top} icon_px={icon_px}')

# Recover icon centres from the row through the middle of the icon band.
band = lum[slot_top + 8:baseline - 8, :]
ink = band > 27
colsum = ink.sum(axis=0)

runs, inrun, start = [], False, 0
for x, v in enumerate(colsum):
    if v > 2 and not inrun:
        start, inrun = x, True
    elif v <= 2 and inrun:
        runs.append((start, x))
        inrun = False
if inrun:
    runs.append((start, len(colsum)))

# Keep runs wide enough to be an icon, then assume uniform pitch 112 to rebuild
# centres (artwork may split one icon into several runs).
runs = [r for r in runs if r[1] - r[0] > 6]
if not runs:
    print('no icon runs found')
    sys.exit(1)

centres = [(r[0] + r[1]) / 2.0 for r in runs]
# Snap to the dominant pitch: take the median gap between well-separated centres.
gaps = [b - a2 for a2, b in zip(centres, centres[1:]) if b - a2 > 60]
pitch = float(np.median(gaps)) if gaps else 112.0
first = centres[0]
n = int(round((w - first) / pitch)) + 1
print(f'first centre {first:.1f} pitch {pitch:.1f} -> {n} candidate slots\n')

print('slot  centre   slot.left  col@left          slot.right col@right')
print('            (mean ink frac over the band)')

for i in range(n):
    cx = first + i * pitch
    sl = cx - icon_px / 2.0
    sr = cx + icon_px / 2.0
    if sl < 2 or sr > w - 2:
        continue
    for edge, name in ((int(round(sl)), 'L'), (int(round(sr)) - 1, 'R')):
        colv = lum[slot_top + 4:baseline - 4, edge]
        frac = float((colv > 27).mean())
        if frac > 0.5:
            print(f'  slot {i:2d} cx={cx:7.1f}  {name} edge x={edge:4d}  '
                  f'ink fraction={frac:.2f}  mean lum={colv.mean():.1f}  <-- vertical stroke')

print('\n--- raw luminance down the left edge of slot 0 and slot 1 ---')
for i in (0, 1, 2):
    cx = first + i * pitch
    for edge in (int(round(cx - icon_px / 2.0)), int(round(cx + icon_px / 2.0)) - 1):
        colv = lum[slot_top:baseline, edge]
        print(f'slot {i} x={edge}: ' + ' '.join(f'{v:3.0f}' for v in colv[::8]))
