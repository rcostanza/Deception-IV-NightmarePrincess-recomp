#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds res/deception.ico, the icon of the game and setup exes (and so of the
desktop shortcut), from square app artwork in any format Pillow reads (PNG, WebP, ...).

    python tools/make_app_icon.py ARTWORK [--full] [--out res/deception.ico] [--preview PNG]

The artwork is an icon tile (rounded or not) on a plain background: the background that touches the
image's corners becomes transparent, and the tile is cropped and centered on a square. Artwork that is
already transparent, or fills the canvas, is kept as it is. --full keeps the whole square image
(artwork whose own background is part of the picture, such as a game's cover tile).
"""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

PORT = Path(__file__).resolve().parent.parent
SIZES = (256, 128, 64, 48, 40, 32, 24, 20, 16)
# How far (sum of channel differences) a background pixel may be from the corner's colour.
BACKGROUND_TOLERANCE = 40


def cut_background(artwork):
    image = artwork.convert('RGBA')
    w, h = image.size
    filled = image.copy()
    for corner in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1)):
        if filled.getpixel(corner)[3]:  # a corner already cleared, or transparent, needs no fill
            ImageDraw.floodfill(filled, corner, (0, 0, 0, 0), thresh=BACKGROUND_TOLERANCE)
    # A slightly softened mask, so the cut edge is smooth once scaled down.
    mask = Image.eval(filled.getchannel('A'), lambda a: 255 if a else 0).filter(ImageFilter.GaussianBlur(1))
    image.putalpha(Image.composite(mask, Image.new('L', image.size, 0), image.getchannel('A')))
    box = image.getchannel('A').point(lambda a: 255 if a > 8 else 0).getbbox() or (0, 0, w, h)
    tile = image.crop(box)
    side = max(tile.size)
    square = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    square.paste(tile, ((side - tile.width) // 2, (side - tile.height) // 2))
    return square


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('artwork', type=Path)
    p.add_argument('--full', action='store_true', help='keep the whole image: no background cut')
    p.add_argument('--out', type=Path, default=PORT / 'res' / 'deception.ico')
    p.add_argument('--preview', type=Path, help='also write the 256 px icon on a dark background, to check the edge')
    a = p.parse_args()
    artwork = Image.open(a.artwork)
    if a.full:
        side = min(artwork.size)
        left, top = (artwork.width - side) // 2, (artwork.height - side) // 2
        icon = artwork.convert('RGBA').crop((left, top, left + side, top + side))
    else:
        icon = cut_background(artwork)
    icon.resize((256, 256), Image.LANCZOS).save(a.out, format='ICO', sizes=[(s, s) for s in SIZES])
    print(f'Wrote {a.out}')
    if a.preview:
        preview = Image.new('RGBA', (256, 256), (40, 40, 40, 255))
        preview.alpha_composite(icon.resize((256, 256), Image.LANCZOS))
        preview.save(a.preview)


if __name__ == '__main__':
    main()
