#!/usr/bin/env python3
"""Captures the board's serial log for N seconds; optional commands are sent after a delay.
   python tools/serial_log.py 20 [--reset] [--send "5:demo"]"""
import glob, sys, time, serial
port = (sorted(glob.glob("/dev/cu.usbmodem*")) or ["/dev/cu.usbmodem1101"])[0]  # the board's USB port (name depends on the Mac port)
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 15
sends = [a.split(":", 1) for a in sys.argv[2:] if ":" in a]
s = serial.Serial(port, 115200, timeout=0.2)
if "--reset" in sys.argv:
    s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False
t0 = time.time(); sent = set()
while time.time() - t0 < secs:
    for i, (t, cmd) in enumerate(sends):
        if i not in sent and time.time() - t0 >= float(t):
            s.write((cmd + "\n").encode()); sent.add(i); print(f">>> {cmd}", flush=True)
    d = s.read(4096)
    if d: sys.stdout.write(d.decode(errors="replace")); sys.stdout.flush()
