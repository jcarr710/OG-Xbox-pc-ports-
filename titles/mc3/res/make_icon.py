# SPDX-License-Identifier: GPL-2.0-or-later
"""Draws res/icon.ico, the program icon (an original night-drive gauge design).
Run with Pillow installed:  python3 make_icon.py"""
import math
from PIL import Image, ImageDraw, ImageFilter

S = 1024
NIGHT, NIGHT2 = (16, 14, 34, 255), (34, 22, 64, 255)
CYAN, MAGENTA, WHITE = (60, 230, 255, 255), (255, 60, 190, 255), (245, 245, 255, 255)

tile = Image.new('RGBA', (S, S), (0, 0, 0, 0))
grad = Image.new('RGBA', (S, S))
g = ImageDraw.Draw(grad)
for y in range(S):                                   # night-sky gradient
    t = y / S
    g.line([(0, y), (S, y)], fill=tuple(int(NIGHT[i] * (1 - t) + NIGHT2[i] * t) for i in range(3)) + (255,))
mask = Image.new('L', (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle((48, 48, S - 48, S - 48), radius=210, fill=255)
tile.paste(grad, (0, 0), mask)

glow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(glow)
cx, cy, r = S // 2, 590, 330
d.arc((cx - r, cy - r, cx + r, cy + r), 150, 390, fill=CYAN, width=70)          # gauge arc
for k in range(9):                                                              # ticks
    a = math.radians(150 + k * 30)
    r1, r2 = r - 95, r - 140 if k % 2 == 0 else r - 115
    d.line([(cx + r1 * math.cos(a), cy + r1 * math.sin(a)), (cx + r2 * math.cos(a), cy + r2 * math.sin(a))],
           fill=WHITE, width=26 if k % 2 == 0 else 16)
a = math.radians(330)                                                           # needle near the top end
tip = (cx + (r - 70) * math.cos(a), cy + (r - 70) * math.sin(a))
d.line([(cx, cy), tip], fill=MAGENTA, width=46)
d.ellipse((cx - 62, cy - 62, cx + 62, cy + 62), fill=MAGENTA)
d.ellipse((cx - 26, cy - 26, cx + 26, cy + 26), fill=NIGHT)
for i, w in enumerate((260, 180)):                                              # road stripes
    y = 860 + i * 52
    d.rounded_rectangle((cx - w // 2, y, cx + w // 2, y + 22), radius=11, fill=(255, 255, 255, 200 - i * 70))

halo = glow.filter(ImageFilter.GaussianBlur(28))
tile = Image.alpha_composite(tile, Image.composite(halo, Image.new('RGBA', (S, S), (0, 0, 0, 0)), mask))
tile = Image.alpha_composite(tile, Image.composite(glow, Image.new('RGBA', (S, S), (0, 0, 0, 0)), mask))
tile.save('icon.ico', format='ICO', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
tile.resize((256, 256), Image.LANCZOS).save('icon.png')
