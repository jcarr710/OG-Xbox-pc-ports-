# SPDX-License-Identifier: GPL-2.0-or-later
"""Draws res/icon.ico, the program icon (an original spray-can design).
Run with Pillow installed:  python3 make_icon.py"""
from PIL import Image, ImageDraw

S = 1024
LIME, LIME_D = (196, 236, 40, 255), (150, 186, 24, 255)
DARK, WHITE = (22, 24, 29, 255), (240, 242, 245, 255)

tile = Image.new('RGBA', (S, S), (0, 0, 0, 0))
ImageDraw.Draw(tile).rounded_rectangle((48, 48, S - 48, S - 48), radius=210, fill=DARK)

can = Image.new('RGBA', (S, S), (0, 0, 0, 0))          # drawn upright, then tilted
c = ImageDraw.Draw(can)
cx, top, bot, w = 470, 360, 860, 330
c.rounded_rectangle((cx - w // 2, top, cx + w // 2, bot), radius=56, fill=LIME)       # body
c.rectangle((cx - w // 2, top, cx + w // 2, top + 80), fill=LIME)                     # square shoulders
c.rectangle((cx - w // 2, top + 160, cx + w // 2, top + 205), fill=DARK)              # label band
c.chord((cx - w // 2, top - 110, cx + w // 2, top + 110), 180, 360, fill=LIME_D)     # dome
c.rounded_rectangle((cx - 62, top - 175, cx + 62, top - 92), radius=20, fill=WHITE)  # cap
c.rectangle((cx + 40, top - 150, cx + 98, top - 118), fill=WHITE)                    # nozzle
tile.alpha_composite(can.rotate(-16, resample=Image.BICUBIC, center=(cx, 600)))

mist = Image.new('RGBA', (S, S), (0, 0, 0, 0))
m = ImageDraw.Draw(mist)
for x, y, r, a in [(690, 205, 30, 255), (770, 160, 24, 230), (745, 260, 20, 220), (835, 215, 17, 200),
                   (820, 120, 14, 180), (870, 290, 12, 160), (690, 135, 13, 190)]:
    m.ellipse((x - r, y - r, x + r, y + r), fill=LIME[:3] + (a,))
tile.alpha_composite(mist)

tile.save('icon.ico', format='ICO', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
tile.resize((256, 256), Image.LANCZOS).save('icon.png')
