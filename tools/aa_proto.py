"""Tries out, on pictures dumped from the PC build, what the emulator could do to the scene
before the game enlarges it: edge anti-aliasing (FXAA), the enlargement itself (bilinear as the
game does it, or bicubic) and sharpening afterwards (contrast adaptive). For judging by eye
before any of it is written for the GPU.

  python tools/aa_proto.py <scene.png> <out.png> [fxaa] [cubic] [cas=<0..1>] [size=<w>x<h>]

The scene is a dump of the game's resolved scene target (linear light, 8 bit); the result is
the enlarged picture, gamma encoded for looking at.
"""
import sys

import numpy as np
from PIL import Image
from scipy import ndimage


def load(path):
    return np.asarray(Image.open(path).convert('RGB'), dtype=np.float32) / 255.0


def luma_of(rgb):
    # Something like perceived brightness, from linear light that may be brighter than white.
    y = rgb @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    return np.sqrt(y / (1.0 + y))


def sample(channel, x, y):
    return ndimage.map_coordinates(channel, [y, x], order=1, mode='nearest')


def fxaa(rgb, threshold=0.125, threshold_min=0.0312, subpix=0.6,
         steps=(1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 2.0, 4.0)):
    h, w, _ = rgb.shape
    luma = luma_of(rgb)
    pad = np.pad(luma, 1, mode='edge')
    m = luma
    n = pad[:-2, 1:-1]
    s = pad[2:, 1:-1]
    wst = pad[1:-1, :-2]
    e = pad[1:-1, 2:]
    nw = pad[:-2, :-2]
    ne = pad[:-2, 2:]
    sw = pad[2:, :-2]
    se = pad[2:, 2:]
    high = np.maximum.reduce([m, n, s, wst, e])
    low = np.minimum.reduce([m, n, s, wst, e])
    span = high - low
    edge = span >= np.maximum(threshold_min, high * threshold)

    horz = (np.abs(-2 * wst + nw + sw) + np.abs(-2 * m + n + s) * 2 + np.abs(-2 * e + ne + se))
    vert = (np.abs(-2 * n + nw + ne) + np.abs(-2 * m + wst + e) * 2 + np.abs(-2 * s + sw + se))
    is_horz = horz >= vert
    # The two sides across the edge.
    side_a = np.where(is_horz, n, wst)
    side_b = np.where(is_horz, s, e)
    grad_a = np.abs(side_a - m)
    grad_b = np.abs(side_b - m)
    a_steeper = grad_a >= grad_b
    gradient = np.maximum(grad_a, grad_b) * 0.25
    sign = np.where(a_steeper, -1.0, 1.0).astype(np.float32)
    pair = np.where(a_steeper, side_a, side_b)
    mid = 0.5 * (pair + m)
    m_below = (m - mid) < 0

    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    # Half a pixel towards the steeper side, then along the edge both ways.
    bx = xx + np.where(is_horz, 0.0, sign * 0.5)
    by = yy + np.where(is_horz, sign * 0.5, 0.0)
    dx = np.where(is_horz, 1.0, 0.0).astype(np.float32)
    dy = 1.0 - dx
    neg = np.zeros_like(m)
    pos = np.zeros_like(m)
    end_n = np.zeros_like(m)
    end_p = np.zeros_like(m)
    done_n = ~edge
    done_p = ~edge
    for step in steps:
        neg = np.where(done_n, neg, neg + step)
        pos = np.where(done_p, pos, pos + step)
        ln = sample(luma, bx - dx * neg, by - dy * neg) - mid
        lp = sample(luma, bx + dx * pos, by + dy * pos) - mid
        end_n = np.where(done_n, end_n, ln)
        end_p = np.where(done_p, end_p, lp)
        done_n = done_n | (np.abs(end_n) >= gradient)
        done_p = done_p | (np.abs(end_p) >= gradient)
        if done_n.all() and done_p.all():
            break
    good_n = (end_n < 0) != m_below
    good_p = (end_p < 0) != m_below
    nearer_n = neg < pos
    dist = np.minimum(neg, pos)
    good = np.where(nearer_n, good_n, good_p)
    offset = np.where(good, 0.5 - dist / np.maximum(neg + pos, 1e-6), 0.0)

    # What a pixel that stands out from all around it is blended by.
    around = (2 * (n + s + wst + e) + nw + ne + sw + se) / 12.0
    sub = np.clip(np.abs(around - m) / np.maximum(span, 1e-6), 0.0, 1.0)
    sub = (sub * sub * (3.0 - 2.0 * sub)) ** 2 * subpix
    blend = np.where(edge, np.maximum(offset, sub), 0.0)

    # The neighbour on the steeper side, mixed in by weights that keep one bright pixel from
    # outshining the other.
    padc = np.pad(rgb, ((1, 1), (1, 1), (0, 0)), mode='edge')
    iy = (np.arange(h)[:, None] + 1 + np.where(is_horz, sign, 0)).astype(np.int64)
    ix = (np.arange(w)[None, :] + 1 + np.where(is_horz, 0, sign)).astype(np.int64)
    other = padc[iy, ix]
    wa = (1.0 - blend) / (1.0 + rgb.max(axis=2))
    wb = blend / (1.0 + other.max(axis=2))
    out = (rgb * wa[..., None] + other * wb[..., None]) / (wa + wb)[..., None]
    return out, edge


def resize(rgb, size, cubic):
    w, h = size
    sh, sw, _ = rgb.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    sx = (xx + 0.5) * sw / w - 0.5
    sy = (yy + 0.5) * sh / h - 0.5
    if not cubic:
        return np.stack([sample(rgb[..., c], sx, sy) for c in range(3)], axis=2)
    # Catmull-Rom, as the GPU's cubic filter does it.
    x0 = np.floor(sx)
    y0 = np.floor(sy)
    fx = sx - x0
    fy = sy - y0

    def weights(t):
        return (-0.5 * t ** 3 + t ** 2 - 0.5 * t, 1.5 * t ** 3 - 2.5 * t ** 2 + 1.0,
                -1.5 * t ** 3 + 2.0 * t ** 2 + 0.5 * t, 0.5 * t ** 3 - 0.5 * t ** 2)

    wx = weights(fx)
    wy = weights(fy)
    out = np.zeros((h, w, 3), dtype=np.float32)
    for j in range(4):
        py = np.clip(y0 + j - 1, 0, sh - 1).astype(np.int64)
        for i in range(4):
            px = np.clip(x0 + i - 1, 0, sw - 1).astype(np.int64)
            out += rgb[py, px] * (wx[i] * wy[j])[..., None]
    return np.clip(out, 0.0, None)


def cas(rgb, amount):
    """Contrast adaptive sharpening (AMD's, in its plain form), on gamma-encoded values."""
    pad = np.pad(rgb, ((1, 1), (1, 1), (0, 0)), mode='edge')
    b = pad[:-2, 1:-1]
    d = pad[1:-1, :-2]
    e = rgb
    f = pad[1:-1, 2:]
    hh = pad[2:, 1:-1]
    low = np.minimum.reduce([b, d, e, f, hh])
    high = np.maximum.reduce([b, d, e, f, hh])
    amp = np.sqrt(np.clip(np.minimum(low, 1.0 - high) / np.maximum(high, 1e-5), 0.0, 1.0))
    peak = -1.0 / (8.0 - 3.0 * amount)
    wgt = amp * peak
    return np.clip((b * wgt + d * wgt + f * wgt + hh * wgt + e) / (1.0 + 4.0 * wgt), 0.0, 1.0)


def encode(rgb):
    return np.clip(rgb, 0.0, 1.0) ** (1.0 / 2.2)


def main():
    scene = load(sys.argv[1])
    out = sys.argv[2]
    options = sys.argv[3:]
    size = (1440, 1536)
    sharpen = 0.0
    for option in options:
        if option.startswith('size='):
            size = tuple(int(v) for v in option[5:].split('x'))
        if option.startswith('cas='):
            sharpen = float(option[4:])
    if 'fxaa' in options:
        scene, edge = fxaa(scene)
        print('edge pixels: %.1f%%' % (100.0 * edge.mean()))
    picture = encode(resize(scene, size, 'cubic' in options))
    if sharpen > 0.0:
        picture = cas(picture, sharpen)
    Image.fromarray((picture * 255.0 + 0.5).astype(np.uint8)).save(out)
    print(out, picture.shape)


main()
