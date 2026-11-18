#!/usr/bin/env python3
"""Generate the test images for the Edge transparency test page (README item 131).

Everything is drawn on a fully transparent canvas, so a browser that ignores or mishandles alpha shows the RGB that is
stored under the transparent pixels (black/white/red variants) instead of the page background.
"""
import os
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 450, 135


def font(size):
    for p in ('/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',
              '/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf'):
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    return ImageFont.load_default()


def logo(under_rgb):
    """A slither.io-like logo: green->purple gradient text with antialiased edges and a soft translucent glow, on a fully
    transparent background whose (invisible) RGB is `under_rgb`."""
    base = Image.new('RGBA', (W, H), under_rgb + (0,))
    # text mask (antialiased alpha)
    mask = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(mask)
    f = font(88)
    d.text((8, 12), 'slither.io', font=f, fill=255)
    # gradient fill green -> purple
    grad = Image.new('RGBA', (W, H))
    gp = grad.load()
    for x in range(W):
        t = x / (W - 1)
        r = int(60 + t * (120 - 60) + (t > 0.7) * 20)
        g = int(230 - t * 140)
        b = int(110 + t * 140)
        for y in range(H):
            gp[x, y] = (r, g, b, 255)
    # translucent glow behind the text (alpha 0..110)
    glow_mask = mask.filter(ImageFilter.GaussianBlur(7)).point(lambda v: int(v * 0.43))
    glow = Image.new('RGBA', (W, H), (20, 20, 20, 0))
    glow.putalpha(glow_mask)
    out = Image.alpha_composite(base, glow)
    text_layer = grad.copy()
    text_layer.putalpha(mask)
    out = Image.alpha_composite(out, text_layer)
    # Pillow's alpha_composite keeps RGB of transparent pixels = under_rgb only where alpha stays 0; enforce it
    px = out.load()
    for y in range(H):
        for x in range(W):
            r, g, b, a = px[x, y]
            if a == 0:
                px[x, y] = under_rgb + (0,)
    return out


for name, rgb in (('black', (0, 0, 0)), ('white', (255, 255, 255)), ('red', (255, 0, 0))):
    im = logo(rgb)
    im.save(os.path.join(HERE, 'logo_under_%s.png' % name))

# the plain one used by the other tests
base = logo((0, 0, 0))
base.save(os.path.join(HERE, 'logo.png'))
base.save(os.path.join(HERE, 'logo.webp'), lossless=True, exact=True)
# GIF with a transparent index
g = base.convert('RGBA')
alpha = g.getchannel('A')
p = g.convert('RGB').convert('P', palette=Image.ADAPTIVE, colors=255)
mask = Image.eval(alpha, lambda a: 255 if a <= 128 else 0)
p.paste(255, mask)
p.save(os.path.join(HERE, 'logo.gif'), transparency=255)

# alpha ramp: solid red with alpha 0..255 left to right, 256x40
ramp = Image.new('RGBA', (256, 40))
rp = ramp.load()
for x in range(256):
    for y in range(40):
        rp[x, y] = (255, 40, 40, x)
ramp.save(os.path.join(HERE, 'ramp.png'))

# palette PNG with tRNS (one transparent colour, one 50% colour)
pal = Image.new('P', (120, 60), 0)
pd = ImageDraw.Draw(pal)
pal.putpalette([0, 0, 0, 0, 200, 80, 255, 255, 0] + [0] * (256 * 3 - 9))
pd.ellipse((5, 5, 60, 55), fill=1)
pd.ellipse((55, 5, 115, 55), fill=2)
pal.save(os.path.join(HERE, 'palette_trns.png'), transparency=bytes([0, 255, 128]))

# SVG logo-ish
open(os.path.join(HERE, 'shape.svg'), 'w').write(
    '<svg xmlns="http://www.w3.org/2000/svg" width="240" height="90" viewBox="0 0 240 90">'
    '<rect x="5" y="5" width="110" height="80" rx="20" fill="#3fdc7a" fill-opacity="0.9"/>'
    '<circle cx="170" cy="45" r="38" fill="#8a4cf0" fill-opacity="0.55"/></svg>')
print('wrote images to', HERE)
