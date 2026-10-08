"""Puts the same parts of the first level's picture side by side as different settings draw it,
from the full-size dumps of the PC build (tools/pc-dump-test.sh, build/dev/dump/<name>/):

  python tools/picture-compare.py <out.png>

What is compared is the picture the game hands to the headset for one eye (1440x1536, enlarged
by the game from whatever size it drew its scene at). What the app does to it afterwards is
done here the same way: up to version 0.6 it was shown 15% smaller while the GPU was busy, from
0.7 on it is sharpened. Each cut is shown three times enlarged, pixel for pixel.
"""
import glob
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'dev', 'dump')


def eye(name):
    path = glob.glob(os.path.join(ROOT, name, '*_523ce0000_1440x1536_R8G8B8A8Srgb_s1.png'))[0]
    linear = np.asarray(Image.open(path).convert('RGB'), dtype=np.float32) / 255.0
    # The dump holds linear light with red and blue the other way round.
    return np.clip(linear[..., ::-1], 0.0, 1.0) ** (1.0 / 2.2)


def shown_smaller(picture, scale=0.855):
    h, w, _ = picture.shape
    image = Image.fromarray((picture * 255.0 + 0.5).astype(np.uint8))
    small = image.resize((round(w * scale), round(h * scale)), Image.BILINEAR)
    return np.asarray(small.resize((w, h), Image.BILINEAR), dtype=np.float32) / 255.0


def sharpened(picture, amount=0.6):
    pad = np.pad(picture, ((1, 1), (1, 1), (0, 0)), mode='edge')
    above, below = pad[:-2, 1:-1], pad[2:, 1:-1]
    left, right = pad[1:-1, :-2], pad[1:-1, 2:]
    darkest = np.minimum.reduce([above, below, left, right, picture])
    brightest = np.maximum.reduce([above, below, left, right, picture])
    room = np.sqrt(np.clip(np.minimum(darkest, 1.0 - brightest) / np.maximum(brightest, 1e-5),
                           0.0, 1.0))
    weight = room * (-1.0 / (8.0 - 3.0 * amount))
    return np.clip(((above + below + left + right) * weight + picture) / (1.0 + 4.0 * weight),
                   0.0, 1.0)


PANELS = [
    ('0.6 in the level: 816x870, edges as drawn, shown 15% smaller', lambda: shown_smaller(eye('l3m1'))),
    ('0.7, heaviest views: 816x870, edges smoothed, sharpened', lambda: sharpened(eye('l3fx'))),
    ('0.7, most of the level: 1200x1280, smoothed, sharpened', lambda: sharpened(eye('l5fx'))),
    ('PlayStation 4 at its smallest: 960x1080, multisampled', lambda: eye('l4')),
    ('PlayStation 4 at its largest: 1440x1536, multisampled', lambda: eye('l6')),
]
# x, y, width, height of each cut, as fractions of the picture.
CUTS = [(0.40, 0.47, 0.11, 0.11), (0.74, 0.20, 0.11, 0.11), (0.05, 0.36, 0.11, 0.11)]
ZOOM = 3


def main():
    out = sys.argv[1]
    pictures = [(label, make()) for label, make in PANELS]
    h, w, _ = pictures[0][1].shape
    cut_w = round(CUTS[0][2] * w) * ZOOM
    cut_h = round(CUTS[0][3] * h) * ZOOM
    gap = 6
    label_h = 30
    sheet = Image.new('RGB', (len(pictures) * (cut_w + gap) - gap,
                              label_h + len(CUTS) * (cut_h + gap) - gap), 'black')
    draw = ImageDraw.Draw(sheet)
    for column, (label, picture) in enumerate(pictures):
        x = column * (cut_w + gap)
        # Two lines of label at most.
        words = label.split(' ')
        line, lines = '', []
        for word in words:
            if len(line) + len(word) + 1 > cut_w // 6:
                lines.append(line)
                line = word
            else:
                line = (line + ' ' + word).strip()
        lines.append(line)
        for i, text in enumerate(lines[:2]):
            draw.text((x + 3, 2 + 13 * i), text, fill='white')
        image = Image.fromarray((picture * 255.0 + 0.5).astype(np.uint8))
        for row, (fx, fy, fw, fh) in enumerate(CUTS):
            box = (round(fx * w), round(fy * h), round(fx * w) + round(fw * w),
                   round(fy * h) + round(fh * h))
            cut = image.crop(box).resize((cut_w, cut_h), Image.NEAREST)
            sheet.paste(cut, (x, label_h + row * (cut_h + gap)))
    sheet.save(out)
    print(out, sheet.size)


main()
