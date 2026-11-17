"""Per-icon geometry inside the dock plate: true bbox, size spread, centring.

The plate's own border is excluded by working strictly inside it, otherwise every
icon reports the full plate height and nothing can be compared.
"""

import sys
import numpy as np
from PIL import Image

path = sys.argv[1]
im = Image.open(path).convert('RGB')
a = np.asarray(im).astype(np.float64)
h, w, _ = a.shape
lum = a.mean(axis=2)

plate = float(np.median(lum[100:180, 20:60]))

# Plate interior band: skip the 6px border stroke top and bottom.
TOP, BOT = 84, 200
ink = lum > plate + 12
band = ink[TOP:BOT, :]

colsum = band.sum(axis=0)
runs, inrun, start = [], False, 0
for x, v in enumerate(colsum):
    if v > 3 and not inrun:
        start, inrun = x, True
    elif v <= 3 and inrun:
        runs.append([start, x])
        inrun = False
if inrun:
    runs.append([start, len(colsum)])

merged = []
for r in runs:
    if merged and r[0] - merged[-1][1] < 12:
        merged[-1][1] = r[1]
    else:
        merged.append(list(r))
merged = [tuple(m) for m in merged if m[1] - m[0] > 14]

print(f'{path}  {w}x{h}  plate luminance={plate:.1f}')
print(f'interior band y {TOP}..{BOT}\n')
print(f'{"idx":>3} {"x0":>5} {"x1":>5} {"w":>4} {"y0":>5} {"y1":>5} {"h":>4} '
      f'{"cx":>7} {"cy":>7} {"fill":>6}')

rows_out = []
for i, (x0, x1) in enumerate(merged):
    sub = band[:, x0:x1]
    ys = np.where(sub.any(axis=1))[0]
    if len(ys) == 0:
        continue
    y0, y1 = TOP + int(ys.min()), TOP + int(ys.max()) + 1
    ww, hh = x1 - x0, y1 - y0
    cx = (x0 + x1) / 2.0
    cy = (y0 + y1) / 2.0
    fill = sub.mean() * 100
    rows_out.append((i, x0, x1, ww, y0, y1, hh, cx, cy, fill))
    print(f'{i:3d} {x0:5d} {x1:5d} {ww:4d} {y0:5d} {y1:5d} {hh:4d} '
          f'{cx:7.1f} {cy:7.1f} {fill:5.1f}%')

if len(rows_out) > 1:
    ws = [r[3] for r in rows_out]
    hs = [r[6] for r in rows_out]
    cys = [r[8] for r in rows_out]
    cxs = [r[7] for r in rows_out]
    print(f'\nwidth  : min={min(ws)} max={max(ws)} spread={max(ws) - min(ws)}')
    print(f'height : min={min(hs)} max={max(hs)} spread={max(hs) - min(hs)}')
    print(f'centre y: min={min(cys):.1f} max={max(cys):.1f} '
          f'spread={max(cys) - min(cys):.1f}')
    gaps = np.diff(cxs)
    print(f'centre x gaps: {[round(float(g), 1) for g in gaps]}')
    print(f'  mean={gaps.mean():.2f} stdev={gaps.std():.2f} '
          f'min={gaps.min():.1f} max={gaps.max():.1f}')
