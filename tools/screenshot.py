#!/usr/bin/env python3
"""Saves what the board's screen shows as a PNG (console command "shot").
   python tools/screenshot.py out.png        (ESP-IDF python, needs pyserial + pillow)"""
import glob, sys, time, serial
from PIL import Image
port = (sorted(glob.glob("/dev/cu.usbmodem*")) or ["/dev/cu.usbmodem1101"])[0]
s = serial.Serial(port, 115200, timeout=0.5)
s.reset_input_buffer()
s.write(b"shot\n")
img = Image.new("RGB", (320, 240))
rows, t0, buf = 0, time.time(), b""
while rows < 240 and time.time() - t0 < 20:
    buf += s.read(65536)
    *lines, buf = buf.split(b"\n")
    for ln in lines:
        ln = ln.strip()
        if not ln.startswith(b"SHOT "):
            continue
        y = int(ln[5:8]); hexs = ln[9:]
        for x in range(320):
            v = int(hexs[x * 4:x * 4 + 4], 16)
            v = ((v & 0xFF) << 8) | (v >> 8)  # stored byte-swapped
            img.putpixel((x, y), ((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3))
        rows += 1
img.save(sys.argv[1] if len(sys.argv) > 1 else "screen.png")
print("rows", rows)
