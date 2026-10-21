"""Compares two folders of the launcher's preview shots, scene by scene: a change that should draw the same (moving
code between files, say) should leave every scene as it was. The shots aren't exactly reproducible: the clock shows the
minute it was taken, and a few scenes are caught mid-animation (cc-editing and cc-options wobble, move-hint lifts a tile,
crossfade fades), which differ by up to about 0.3% of their pixels between two runs of one build. So the clock's band is
left out, and a scene passes while fewer than 0.5% of its pixels differ visibly (by more than 16 of 255).
   python3 tools/native-preview/compare-screens.py <folder before> <folder after>
"""
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw

CLOCK = (1580, 40, 1880, 120)  # the clock capsule, top right (TOP_Y 52, BAR 56, ending at RIGHT 1872)
LIMIT = 0.005

before, after = Path(sys.argv[1]), Path(sys.argv[2])
problems, scenes = [], sorted(before.glob('native-*.png'))
if not scenes:
    sys.exit(f'  ✗ no screens in {before}')
for path in scenes:
    name = path.name[len('native-'):-len('.png')]
    other = after / path.name
    if not other.exists():
        problems.append(f'{name}: not in {after}')
        continue
    diff = ImageChops.difference(Image.open(path).convert('RGB'), Image.open(other).convert('RGB')).convert('L')
    ImageDraw.Draw(diff).rectangle(CLOCK, fill=0)
    hist = diff.histogram()
    share = sum(hist[17:]) / sum(hist)
    if share:
        box = diff.point(lambda v: 255 if v > 16 else 0).getbbox()
        print(f'  {name}: {share:.3%} of its pixels differ, in {box}')
    if share >= LIMIT:
        problems.append(f'{name}: {share:.2%} of its pixels differ')

if problems:
    sys.exit('\n'.join(f'  ✗ {p}' for p in problems))
print(f'{len(scenes)} screens: the same')
