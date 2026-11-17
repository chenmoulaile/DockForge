"""Crop and upscale a region of a screenshot so a vision model can read detail.

Usage: crop_zoom.py <src> <dst> <x0> <y0> <x1> <y1> [scale]
Writes to the DSH vision-allowed directory so image_understand can open it.
"""

import sys
import numpy as np
from PIL import Image

src, dst = sys.argv[1], sys.argv[2]
x0, y0, x1, y1 = (int(v) for v in sys.argv[3:7])
scale = float(sys.argv[7]) if len(sys.argv) > 7 else 4.0

im = Image.open(src).convert('RGB')
im = im.crop((x0, y0, x1, y1))
im = im.resize((int(im.width * scale), int(im.height * scale)), Image.NEAREST)
im.save(dst)
print(f'{dst}  {im.width}x{im.height}  (crop {x1 - x0}x{y1 - y0} at {scale}x)')
