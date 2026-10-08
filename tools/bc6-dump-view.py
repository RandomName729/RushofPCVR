# Turns the raw dump of a BC6H texture array (SHADPS4_DUMP_IMAGE, see vk_presenter.cpp) into a
# contact sheet: a few layers (rows) at every level (columns), tone-mapped, plus the average
# brightness of every layer and level - to see whether levels of a texture hold what they should.
#   python tools/bc6-dump-view.py <dump folder> <address hex> <out.png> [layers to show]
import glob
import os
import re
import sys

import numpy as np
import texture2ddecoder
from PIL import Image

folder, address, out = sys.argv[1], sys.argv[2], sys.argv[3]
show = [int(x) for x in sys.argv[4].split(',')] if len(sys.argv) > 4 else [0, 1, 2, 3, 4, 5, 6, 7, 30, 31, 186, 191]
files = sorted(glob.glob(os.path.join(folder, 'image_%s_level*_Bc6H*.bin' % address)),
               key=lambda f: int(re.search(r'level(\d+)', f).group(1)))
cell = 96
sheet = Image.new('RGB', (cell * len(files), cell * len(show)), (40, 40, 40))
for col, f in enumerate(files):
    level = int(re.search(r'level(\d+)', f).group(1))
    w, h, layers = map(int, re.search(r'_(\d+)x(\d+)x(\d+)_', f).groups())
    data = open(f, 'rb').read()
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    per_layer = bw * bh * 16
    means = []
    for layer in range(layers):
        raw = data[layer * per_layer:(layer + 1) * per_layer]
        # The decoder gives BGRA bytes after tone mapping its half floats; good enough to see.
        px = texture2ddecoder.decode_bc6(raw, bw * 4, bh * 4)
        arr = np.frombuffer(px, dtype=np.uint8).reshape(bh * 4, bw * 4, 4)[:h, :w, :3][:, :, ::-1]
        means.append(float(arr.mean()))
        if layer in show:
            img = Image.fromarray(arr.copy()).resize((cell, cell), Image.NEAREST)
            sheet.paste(img, (col * cell, show.index(layer) * cell))
    m = np.array(means)
    print('level %d (%dx%d): mean of layers 0-191 %.1f, 192-255 %.1f, first 8: %s' % (
        level, w, h, m[:192].mean(), m[192:].mean() if layers > 192 else -1,
        ' '.join('%.0f' % x for x in m[:8])))
sheet.save(out)
print('sheet', out)
