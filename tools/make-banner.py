#!/usr/bin/env python3
"""Draws the README's banner and GitHub's social preview: a screenshot from the TV in a TV's frame, most of
the picture, on a dark ground in the screenshot's light. Writes docs/banner.jpg (2400 x 1660, Home+'s name
over the TV) and docs/social-preview.jpg (1280 x 640, the TV alone: the picture a link to the repo shows,
set in the repo's Settings -> Social preview).
Needs Pillow:  python3 tools/make-banner.py   (or in the preview container)
"""
import os
from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SHOT = os.path.join(ROOT, 'docs/screenshots/home-clear.jpg')
FONTS = os.path.join(ROOT, 'native/assets/fonts')

LABEL = 'A HOME SCREEN FOR ROOTED LG TVS'
NAME = 'Home+'

GROUND = (11, 12, 15)
BLUE = (138, 176, 255)  # the icon's, lighter on the dark ground
WHITE = (245, 245, 247)


def font(name, px):
    return ImageFont.truetype(os.path.join(FONTS, 'Inter-%s.ttf' % name), round(px))


def spaced(d, x, baseline, text, f, fill, space):
    """Text with room between its letters, which Pillow doesn't do."""
    for ch in text:
        d.text((x, baseline), ch, font=f, fill=fill, anchor='ls')
        x += d.textlength(ch, font=f) + space


def light(size, box, colour, alpha, r):
    """A soft light: an ellipse, blurred."""
    im = Image.new('RGBA', size, colour + (0,))
    ImageDraw.Draw(im).ellipse(box, fill=colour + (alpha,))
    return im.filter(ImageFilter.GaussianBlur(r))


def banner(w, h, tv, top=None, words=False, k=2):
    """w x h, laid out by the width: the TV tv thousandths of it wide, its top `top` thousandths of it down
    (None: in the middle), and with words the label and the name above it. Drawn k times the size, then
    scaled down: smooth edges."""
    W, H, u = w * k, h * k, w * k / 1000
    im = Image.new('RGBA', (W, H), GROUND + (255,))
    shot = Image.open(SHOT).convert('RGB')

    # the TV: a 16:9 screen in a thin frame, centred
    tw = round(tv * u)
    bezel, r = round(0.012 * tw), round(0.013 * tw)
    sw = tw - 2 * bezel
    sh = round(sw * 9 / 16)
    th = sh + 2 * bezel
    tx, ty = (W - tw) // 2, (H - th) // 2 if top is None else round(top * u)

    # around it the picture's own colour, as the light of a TV with ambient LEDs; a cooler light at the top
    # right; its shadow
    avg = shot.resize((1, 1), Image.LANCZOS).getpixel((0, 0))
    im.alpha_composite(light((W, H), (tx - 0.06 * tw, ty + 0.04 * th, tx + 1.06 * tw, ty + 1.04 * th), avg, 150, 80 * u))
    im.alpha_composite(light((W, H), (0.58 * W, -0.45 * W, 1.3 * W, 0.2 * W), (64, 84, 124), 70, 90 * u))
    shadow = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).rounded_rectangle((tx, ty + 12 * u, tx + tw, ty + th + 12 * u), r, fill=(0, 0, 0, 160))
    im.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(18 * u)))

    d = ImageDraw.Draw(im)
    if ty + th + 12 * u < H:  # the TV whole: a stand, barely there
        st = 0.34 * tw
        d.rounded_rectangle(((W - st) / 2, ty + th - 4 * u, (W + st) / 2, ty + th + 9 * u), 3 * u, fill=(22, 23, 27, 255))
    d.rounded_rectangle((tx, ty, tx + tw, ty + th), r, fill=(29, 30, 34, 255), outline=(60, 62, 68, 255), width=max(2, round(1.1 * u)))
    mask = Image.new('L', (sw, sh), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, sw - 1, sh - 1), round(2.5 * u), fill=255)
    im.paste(shot.resize((sw, sh), Image.LANCZOS), (tx + bezel, ty + bezel), mask)

    if words:  # the label and the name above it, in line with its left edge
        lf, nf = font('SemiBold', 9.8 * u), font('Bold', 44 * u)
        spaced(d, tx, 76.5 * u, LABEL, lf, BLUE, 0.2 * lf.size)
        d.text((tx - 0.03 * nf.size, 118.5 * u), NAME, font=nf, fill=WHITE, anchor='ls')
    return im.resize((w, h), Image.LANCZOS).convert('RGB')


if __name__ == '__main__':
    banner(2400, 1660, 880, 139, words=True).save(os.path.join(ROOT, 'docs/banner.jpg'), quality=88, optimize=True, progressive=True)
    # GitHub's repo card shows the picture's middle three quarters (its top and bottom cut off), and the repo's
    # name under it: no words, and the whole screen, from its top bar to the dock, inside that band
    banner(1280, 640, 720).save(os.path.join(ROOT, 'docs/social-preview.jpg'), quality=90, optimize=True, progressive=True)
