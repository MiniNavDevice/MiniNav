#!/usr/bin/env python3
"""Renders the MiniNav logo (the boot animation's last frame) as PNGs into docs/:
logo-day.png, logo-night.png (1280x960, boot-screen background) and logo-transparent.png."""
import math, os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
TTF = os.path.join(HERE, "fonts", "Nunito.ttf")
OUT = os.path.join(HERE, "..", "docs")
K = 4                      # 320x240 design scaled 4x
SS = 3                     # extra supersampling for smooth edges
PAL = {
    "day": dict(bg0=(244, 247, 251), bg1=(226, 235, 247), text=(35, 48, 74), accent=(74, 127, 224), pin=(240, 110, 100)),
    "night": dict(bg0=(16, 25, 43), bg1=(27, 39, 64), text=(230, 235, 245), accent=(122, 167, 255), pin=(255, 138, 128)),
}


def render(theme, transparent=False):
    p = PAL[theme]
    s = K * SS
    W, H = 320 * s, 240 * s
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    if not transparent:
        for y in range(H):
            t = y / (H - 1)
            d.line([(0, y), (W, y)], fill=tuple(int(p["bg0"][i] + (p["bg1"][i] - p["bg0"][i]) * t) for i in range(3)))
    P = [(-10, 235), (40, 150), (110, 100), (160, 100)]
    def bez(t):
        u = 1 - t
        return tuple((u**3 * P[0][k] + 3 * u * u * t * P[1][k] + 3 * u * t * t * P[2][k] + t**3 * P[3][k]) * s for k in (0, 1))
    r = 3.5 * s   # stamp discs along the curve: a clean round stroke with no join notches
    for i in range(2401):
        q = bez(i / 2400)
        d.ellipse([q[0] - r, q[1] - r, q[0] + r, q[1] + r], fill=p["accent"])
    # teardrop pin: head circle + tangent lines to the tip
    cx, cy, R, tip = 160 * s, 72 * s, 17 * s, 100 * s
    dist = tip - cy
    a = math.asin(R / dist)
    tx = R * math.cos(a)
    ty = cy + R * math.sin(a)
    d.polygon([(cx, tip), (cx - tx, ty), (cx + tx, ty)], fill=p["pin"])
    d.ellipse([cx - R, cy - R, cx + R, cy + R], fill=p["pin"])
    d.ellipse([cx - 6 * s, cy - 6 * s, cx + 6 * s, cy + 6 * s], fill=(255, 255, 255))
    f = ImageFont.truetype(TTF, 34 * s)
    f.set_variation_by_axes([800])
    wm, wn = d.textlength("Mini", font=f), d.textlength("Nav", font=f)
    x, y = (W - wm - wn) / 2, 166 * s
    d.text((x, y), "Mini", font=f, fill=p["text"], anchor="ls")
    d.text((x + wm, y), "Nav", font=f, fill=p["accent"], anchor="ls")
    return im.resize((320 * K, 240 * K), Image.LANCZOS)


os.makedirs(OUT, exist_ok=True)
render("day").convert("RGB").save(os.path.join(OUT, "logo-day.png"))
render("night").convert("RGB").save(os.path.join(OUT, "logo-night.png"))
t = render("day", transparent=True)
t.crop(t.getbbox()).save(os.path.join(OUT, "logo-transparent.png"))
print("ok")
