"""Print a coarse ASCII map of a screenshot so structure can be read without eyes.

Each cell is the mean luminance of a block of pixels, mapped to a character ramp.
Also prints a column-brightness profile, which is how icon slots are counted.
"""

import sys
import numpy as np
from PIL import Image

path = sys.argv[1]
cols = int(sys.argv[2]) if len(sys.argv) > 2 else 150
rows = int(sys.argv[3]) if len(sys.argv) > 3 else 40

im = Image.open(path).convert('RGB')
a = np.asarray(im).astype(np.float64)
h, w, _ = a.shape
lum = a.mean(axis=2)
print(f'{path}  {w}x{h}')

ramp = ' .:-=+*#%@'
bh, bw = h / rows, w / cols
lines = []
for r in range(rows):
    y0, y1 = int(r * bh), max(int(r * bh) + 1, int((r + 1) * bh))
    line = ''
    for c in range(cols):
        x0, x1 = int(c * bw), max(int(c * bw) + 1, int((c + 1) * bw))
        v = lum[y0:y1, x0:x1].mean()
        idx = min(len(ramp) - 1, int(v / 255.0 * len(ramp)))
        line += ramp[idx]
    lines.append(f'{y0:4d} |{line}|')
print('\n'.join(lines))

print('\n--- column profile (mean luminance per 16px bucket) ---')
bucket = 16
prof = [lum[:, x:x + bucket].mean() for x in range(0, w, bucket)]
for i in range(0, len(prof), 12):
    chunk = prof[i:i + 12]
    xs = ' '.join(f'{x:5.1f}' for x in chunk)
    start = i * bucket
    print(f'x={start:5d}  {xs}')
