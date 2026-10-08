"""Cuts the same region out of several pictures and puts the cuts side by side, enlarged without
smoothing, to compare what a setting does to the picture.

  python tools/crop.py <out.png> <x,y,w,h as fractions of the picture> <zoom> <picture> [<picture> ...]

A picture may be given as <label>=<path>. The region is in fractions (0..1) so that pictures of
different sizes show the same part of the scene; each cut is brought to the size of the first
picture's cut times the zoom (nearest neighbour for the first, so its pixels stay pixels, and for
the others too when their size is the same).
"""
import sys

from PIL import Image, ImageDraw


def main():
    out = sys.argv[1]
    fx, fy, fw, fh = (float(v) for v in sys.argv[2].split(','))
    zoom = float(sys.argv[3])
    cuts = []
    size = None
    for item in sys.argv[4:]:
        label, _, path = item.rpartition('=')
        picture = Image.open(path).convert('RGB')
        w, h = picture.size
        box = (round(fx * w), round(fy * h), round((fx + fw) * w), round((fy + fh) * h))
        cut = picture.crop(box)
        if size is None:
            size = (round(cut.size[0] * zoom), round(cut.size[1] * zoom))
        cut = cut.resize(size, Image.NEAREST)
        cuts.append((label or path.split('/')[-1][:24], cut))
    sheet = Image.new('RGB', (size[0] * len(cuts) + 4 * (len(cuts) - 1), size[1] + 18), 'black')
    draw = ImageDraw.Draw(sheet)
    for i, (label, cut) in enumerate(cuts):
        x = i * (size[0] + 4)
        sheet.paste(cut, (x, 18))
        draw.text((x + 3, 3), label, fill='white')
    sheet.save(out)
    print(out, sheet.size)


main()
