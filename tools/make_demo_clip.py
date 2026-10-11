#!/usr/bin/env python3
"""Renders the built-in demo clip: an Android Auto style Google Maps drive at 800x480 with the
UI in the middle 640x480 (80 px black margins, as the phone sends it), encoded H.264 baseline.

    python3 -m pip install --user pillow imageio-ffmpeg
    python3 tools/make_demo_clip.py            # writes main/demo.h264
"""
import math, os, random, subprocess, sys
from PIL import Image, ImageDraw, ImageFont
import imageio_ffmpeg

W, H, MARGIN = 800, 480, 80
UI_W = W - 2 * MARGIN
FPS, SECONDS = 30, 10
OUT = os.path.join(os.path.dirname(__file__), "..", "main", "demo.h264")

# Google Maps day palette (2024)
LAND, PARK, WATER = (245, 241, 232), (196, 232, 193), (156, 192, 249)
ROAD, ROAD_EDGE, MAJOR, MAJOR_EDGE = (255, 255, 255), (218, 220, 224), (253, 226, 147), (241, 196, 102)
BLOCK = (232, 228, 220)
ROUTE, ROUTE_EDGE = (26, 115, 232), (16, 82, 180)
GREEN = (15, 157, 88)

def font(size, bold=True):
    for path in ["/System/Library/Fonts/Supplemental/Arial Bold.ttf" if bold else "/System/Library/Fonts/Supplemental/Arial.ttf",
                 "/System/Library/Fonts/Helvetica.ttc"]:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()

# ---- static city map ------------------------------------------------------------------------
S = 3000
GRID = 260
random.seed(7)
city = Image.new("RGB", (S, S), LAND)
d = ImageDraw.Draw(city)
for gx in range(0, S, GRID):
    for gy in range(0, S, GRID):
        r = random.random()
        box = [gx + 22, gy + 22, gx + GRID - 22, gy + GRID - 22]
        if r < 0.12:
            d.rounded_rectangle(box, 18, fill=PARK)
        else:
            for _ in range(random.randint(3, 7)):  # buildings
                bx, by = random.randint(box[0], box[2] - 50), random.randint(box[1], box[3] - 50)
                d.rectangle([bx, by, bx + random.randint(25, 60), by + random.randint(25, 60)], fill=BLOCK)
d.polygon([(0, 1900), (S, 1500), (S, 1700), (0, 2130)], fill=WATER)  # river
for i, g in enumerate(range(0, S + 1, GRID)):
    major = i % 3 == 0
    wdt = 30 if major else 18
    for line in ([(g, 0), (g, S)], [(0, g), (S, g)]):
        d.line(line, fill=MAJOR_EDGE if major else ROAD_EDGE, width=wdt + 6)
for i, g in enumerate(range(0, S + 1, GRID)):
    major = i % 3 == 0
    wdt = 30 if major else 18
    for line in ([(g, 0), (g, S)], [(0, g), (S, g)]):
        d.line(line, fill=MAJOR if major else ROAD, width=wdt)

# route along the grid: right, up, right, up...
pts = [(GRID * 2, GRID * 10)]
for dx, dy in [(3, 0), (0, -2), (2, 0), (0, -3), (3, 0), (0, -2)]:
    x, y = pts[-1]
    pts.append((x + dx * GRID, y + dy * GRID))
d.line(pts, fill=ROUTE_EDGE, width=26, joint="curve")
d.line(pts, fill=ROUTE, width=18, joint="curve")

seg_len = [math.dist(pts[i], pts[i + 1]) for i in range(len(pts) - 1)]
total = sum(seg_len)

def at(s):
    for i, L in enumerate(seg_len):
        if s <= L:
            t = s / L
            (x0, y0), (x1, y1) = pts[i], pts[i + 1]
            return x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, math.atan2(y1 - y0, x1 - x0), i, L - s
        s -= L
    (x0, y0), (x1, y1) = pts[-2], pts[-1]
    return x1, y1, math.atan2(y1 - y0, x1 - x0), len(seg_len) - 1, 0

def next_turn(i):
    if i + 1 >= len(pts) - 1:
        return None
    (x0, y0), (x1, y1), (x2, y2) = pts[i], pts[i + 1], pts[i + 2]
    cross = (x1 - x0) * (y2 - y1) - (y1 - y0) * (x2 - x1)
    return "right" if cross > 0 else "left"

STREETS = ["MG Road", "Brigade Rd", "Residency Rd", "Church St", "Lavelle Rd", "Cubbon Rd", "St Marks Rd"]

def turn_arrow(dr, cx, cy, side, col):
    s = 1 if side == "right" else -1
    dr.line([(cx, cy + 22), (cx, cy - 4)], fill=col, width=8)
    dr.line([(cx, cy - 4), (cx + s * 18, cy - 4)], fill=col, width=8)
    dr.polygon([(cx + s * 30, cy - 4), (cx + s * 16, cy - 16), (cx + s * 16, cy + 8)], fill=col)

f_big, f_mid, f_small = font(30), font(20), font(17, bold=False)

# ---- frames ----------------------------------------------------------------------------------
ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
enc = subprocess.Popen([ffmpeg, "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                        "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-",
                        "-c:v", "libx264", "-profile:v", "baseline", "-level", "3.1", "-pix_fmt", "yuv420p",
                        "-preset", "medium", "-g", "60", "-bf", "0", "-b:v", "1500k", "-maxrate", "2500k",
                        "-bufsize", "2500k", "-x264-params", "slices=1:sliced-threads=0:aud=0",
                        "-f", "h264", OUT], stdin=subprocess.PIPE)

n = FPS * SECONDS
speed = (total - 400) / n
heading = None
for k in range(n):
    s = 100 + k * speed
    x, y, ang, seg, left_in_seg = at(s)
    target = ang + math.pi / 2  # rotate so the travel direction points up
    if heading is None:
        heading = target
    diff = (target - heading + math.pi) % (2 * math.pi) - math.pi
    heading += diff * 0.2
    # affine map from screen (640x480 UI, car at 50%, 70%) back into the city image
    cx, cy = UI_W * 0.5, H * 0.70
    zoom = 1.25
    c, sn = math.cos(heading), math.sin(heading)
    a, b = c / zoom, -sn / zoom
    dd, e = sn / zoom, c / zoom
    view = city.transform((UI_W, H), Image.AFFINE,
                          (a, b, x - a * cx - b * cy, dd, e, y - dd * cx - e * cy), resample=Image.BILINEAR)
    v = ImageDraw.Draw(view)
    # car chevron
    v.polygon([(cx, cy - 18), (cx + 13, cy + 13), (cx, cy + 5), (cx - 13, cy + 13)], fill=(255, 255, 255))
    v.polygon([(cx, cy - 13), (cx + 9, cy + 9), (cx, cy + 3), (cx - 9, cy + 9)], fill=ROUTE)
    # left rail (Android Auto launcher bar)
    v.rectangle([0, 0, 64, H], fill=(32, 33, 36))
    for i, col in enumerate([(66, 133, 244), (52, 168, 83), (251, 188, 4), (234, 67, 53)]):
        v.ellipse([18, 70 + i * 70, 46, 98 + i * 70], fill=col)
    v.text((14, 18), "%d:%02d" % (9 + (k // 900) % 3, (41 + k // 30 // 60) % 60), font=f_small, fill=(232, 234, 237))
    # turn card
    side = next_turn(seg)
    v.rounded_rectangle([76, 10, 340, 96], 14, fill=GREEN)
    if side:
        turn_arrow(v, 110, 54, side, (255, 255, 255))
        dist = max(10, int(left_in_seg / zoom / 2) // 10 * 10)
        v.text((150, 18), f"{dist} m", font=f_big, fill=(255, 255, 255))
        v.text((150, 58), STREETS[(seg + 1) % len(STREETS)], font=f_mid, fill=(255, 255, 255))
    else:
        v.text((100, 30), "Destination ahead", font=f_mid, fill=(255, 255, 255))
    # ETA card
    remain = (total - s) / zoom / 2
    v.rounded_rectangle([76, H - 70, UI_W - 12, H - 12], 14, fill=(255, 255, 255))
    v.text((96, H - 58), "%d min" % max(1, int(remain / 450)), font=f_big, fill=GREEN)
    v.text((240, H - 52), "%.1f km  ·  ETA 9:%02d" % (remain / 1000, (52 + int(remain / 450)) % 60), font=f_mid, fill=(95, 99, 104))

    frame = Image.new("RGB", (W, H), (0, 0, 0))
    frame.paste(view, (MARGIN, 0))
    enc.stdin.write(frame.tobytes())
    if k % 60 == 0:
        print(f"frame {k}/{n}", file=sys.stderr)

enc.stdin.close()
enc.wait()
print(f"wrote {OUT}: {os.path.getsize(OUT) // 1024} KB, {n} frames")
