"""The AppImage icon: drawn here, so no game art is shipped. A jungle-green tile,
a setting sun and two palm fronds."""
import sys
from PIL import Image, ImageDraw

S = 256
img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
for y in range(S):                                   # dusk sky to jungle floor
    t = y / S
    c = (int(250 - 170 * t), int(170 - 60 * t), int(60 + 10 * t), 255)
    d.line([(0, y), (S, y)], fill=c)
mask = Image.new('L', (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=48, fill=255)
d.ellipse([78, 70, 178, 170], fill=(255, 214, 90, 255))           # the sun
d.rectangle([0, 176, S, S], fill=(34, 92, 40, 255))                 # the ground
d.polygon([(122, 250), (132, 250), (138, 120), (128, 118)], fill=(92, 58, 30, 255))   # the trunk
for pts in ([(130, 118), (40, 110), (20, 140), (70, 124)], [(130, 118), (220, 96), (240, 128), (184, 116)],
            [(130, 118), (70, 60), (40, 70), (96, 96)], [(130, 118), (196, 56), (222, 66), (170, 98)],
            [(130, 118), (130, 40), (112, 44), (122, 90)]):
    d.polygon(pts, fill=(46, 140, 58, 255))
out = Image.new('RGBA', (S, S), (0, 0, 0, 0)); out.paste(img, (0, 0), mask)
out.save(sys.argv[1])
