"""Checks the launcher's preview shots: every scene run.sh renders was saved, at 1920x1080, isn't
blank, and (but for the home screen) isn't the home screen again, which would mean the keys did nothing.
   python3 tools/native-preview/check-screens.py <folder run.sh saved them in>
"""
import re
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

out = Path(sys.argv[1])
scenes = re.findall(r'^shot ([\w-]+) ', (Path(__file__).parent / 'run.sh').read_text(), re.M)
problems, home = [], None
for name in scenes:
    path = out / f'native-{name}.png'
    if not path.exists():
        problems.append(f'{name}: no screenshot')
        continue
    img = Image.open(path).convert('RGB')
    if img.size != (1920, 1080):
        problems.append(f'{name}: {img.size[0]}x{img.size[1]}, not 1920x1080')
        continue
    if ImageStat.Stat(img.convert('L')).stddev[0] < 8:
        problems.append(f'{name}: blank')
    if name == 'home':
        home = img
    elif home is not None:
        # the share of pixels that visibly changed: the smallest scene (move-hint, a hint pill and an
        # outline) changes about 0.5%, the clock ticking over about 0.01%
        diff = ImageChops.difference(img, home).convert('L').histogram()
        if sum(diff[17:]) / sum(diff) < 0.001:
            problems.append(f'{name}: the same as the home screen (did the keys reach the launcher?)')

if problems:
    sys.exit('\n'.join(f'  ✗ {p}' for p in problems))
print(f'{len(scenes)} screens: ok')
