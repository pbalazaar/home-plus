"""Fake app icons and Stremio artwork for the native preview harness."""
import sys, os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
font = ImageFont.truetype(sys.argv[2], 120)
APPS = [("netflix", "Netflix", (229, 9, 20), "N"), ("youtube", "YouTube", (255, 0, 0), "▶"),
        ("stremio", "Stremio", (123, 91, 245), "S"), ("prime", "Prime Video", (0, 168, 225), "p"),
        ("disney", "Disney+", (17, 60, 207), "D+"), ("plex", "Plex", (229, 160, 13), "›"),
        ("spotify", "Spotify - Music and Podcasts", (30, 215, 96), "♪"), ("twitch", "Twitch", (145, 70, 255), "t"),
        ("com.webos.app.hdmi1", "HDMI 1", (58, 58, 64), "1"), ("browser", "Web Browser", (0, 122, 255), "◎"),
        ("com.palm.app.settings", "Settings", (142, 142, 147), "⚙"), ("jellyfin", "Jellyfin", (170, 92, 195), "J"),
        ("kodi", "Kodi", (23, 178, 231), "K"), ("music", "Music", (252, 60, 68), "♫")]
for i, (aid, title, rgb, glyph) in enumerate(APPS):
    im = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, 255, 255], radius=56, fill=rgb + (255,))
    d.text((128, 132), glyph, font=font, fill="white", anchor="mm")
    im.save(f"{out}/{aid}.png")
print(",".join(f"{a}|{t}" for a, t, _, _ in APPS))
SHOWS = [((15, 42, 68), (59, 123, 191)), ((42, 15, 51), (192, 80, 143)), ((19, 48, 31), (79, 174, 109)),
         ((51, 32, 15), (208, 138, 58)), ((43, 15, 15), (191, 59, 59))]
for i, (a, b) in enumerate(SHOWS):
    im = Image.new("RGB", (1280, 720))
    d = ImageDraw.Draw(im)
    for y in range(720):
        t = y / 719
        d.line([(0, y), (1280, y)], fill=tuple(int(a[c] + (b[c] - a[c]) * t) for c in range(3)))
    d.ellipse([820, 120, 1160, 460], fill=tuple(min(255, c + 40) for c in b))
    im = im.filter(ImageFilter.GaussianBlur(6))
    im.save(f"{out}/show{i}.jpg", quality=88)
# A QR-code-like picture for the sign-in sheets (not a real code)
import random
random.seed(7)
N, C = 29, 12
qr = Image.new("L", (N * C, N * C), 255)
d = ImageDraw.Draw(qr)
for y in range(N):
    for x in range(N):
        if random.random() < 0.45:
            d.rectangle([x * C, y * C, x * C + C - 1, y * C + C - 1], fill=0)
for fx, fy in ((0, 0), (N - 7, 0), (0, N - 7)):  # finder patterns
    d.rectangle([fx * C, fy * C, (fx + 7) * C - 1, (fy + 7) * C - 1], fill=0)
    d.rectangle([(fx + 1) * C, (fy + 1) * C, (fx + 6) * C - 1, (fy + 6) * C - 1], fill=255)
    d.rectangle([(fx + 2) * C, (fy + 2) * C, (fx + 5) * C - 1, (fy + 5) * C - 1], fill=0)
qr.save(f"{out}/qr.png")
# plex.tv's is white on transparency instead (for a dark page): the same picture, as Plex sends it
Image.merge("LA", (Image.new("L", qr.size, 255), qr.point(lambda v: 255 - v))).save(f"{out}/qr-plex.png")
# Title logos, as TMDB serves them: white lettering on transparency
logo_font = ImageFont.truetype(sys.argv[2], 92)
for i, name in enumerate(["NORTHERN LIGHTS", "THE LONG ORBIT", "PAPER MOON"]):
    w = int(logo_font.getlength(name)) + 24
    im = Image.new("RGBA", (w, 120), (0, 0, 0, 0))
    ImageDraw.Draw(im).text((12, 60), name, font=logo_font, fill=(255, 255, 255, 255), anchor="lm")
    im.save(f"{out}/logo{i}.png")
