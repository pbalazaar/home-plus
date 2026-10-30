"""The pictures net_test.c asks its stand-in service for: solid colours, so they're small files that
decode big. usage: make-fixtures.py <dir>"""
import os
import sys

from PIL import Image

out = sys.argv[1]
os.makedirs(out, exist_ok=True)


def solid(name, w, h, rgb):
    Image.new('RGB', (w, h), rgb).save(os.path.join(out, name))


solid('big.png', 2048, 2048, (40, 80, 120))    # 16 MB decoded: the allowance for pictures waiting
solid('huge.png', 4000, 4000, (120, 40, 80))   # 61 MB decoded, and its 1920 x 1920 copy: past the decode allowance
solid('uhd.png', 3840, 2160, (80, 120, 40))    # TMDB's largest: decoded, and kept at 1920 x 1080
solid('hd.png', 1920, 1080, (60, 60, 60))      # 8 MB decoded
for n in 'abcdxyz':
    solid(f'{n}.png', 64, 64, (10, 20, 30))
with open(os.path.join(out, 'hd.png'), 'rb') as f:
    data = f.read()
with open(os.path.join(out, 'truncated.png'), 'wb') as f:
    f.write(data[:len(data) // 2])
with open(os.path.join(out, 'garbage.bin'), 'wb') as f:
    f.write(b'not a picture ' * 100)
with open(os.path.join(out, 'empty.gif'), 'wb') as f:  # no pixels, which stb decodes: 0 x 0, with an image of 0 x 0
    f.write(b'GIF89a' + bytes([0, 0, 0, 0, 0x80, 0, 0, 0, 0, 0, 255, 255, 255]) + b',' + bytes(9) + bytes([2, 2, 0x4c, 1, 0]) + b';')
