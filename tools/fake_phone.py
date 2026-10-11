#!/usr/bin/env python3
"""Plays the phone side of Android Auto over the USB serial port, against a firmware built with
CONFIG_AAM_SERIAL_TRANSPORT=y. Runs the real sequence: version exchange, TLS (the board is the
client), service discovery, channel opens, sensors, video setup/start, then streams
main/demo.h264 as fragmented, separately encrypted media messages, paced by the board's ACKs.

    python tools/fake_phone.py [seconds]          (use the ESP-IDF python, it has pyserial)
"""
import glob, os, ssl, struct, subprocess, sys, tempfile, time
import serial

PORT = (sorted(glob.glob("/dev/cu.usbmodem*")) or ["/dev/cu.usbmodem1101"])[0]  # the board's USB port (name depends on the Mac port)
SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 30
# --go-quiet: after streaming, fall silent like a phone that stalls, and check that the board
# ends the session with a bye-bye instead of just dropping the socket.
GO_QUIET = "--go-quiet" in sys.argv
CLIP = os.path.join(os.path.dirname(__file__), "..", "main", "demo.h264")

F_FIRST, F_LAST, F_CONTROL, F_ENC = 1, 2, 4, 8

# ---------------------------------------------------------------- protobuf helpers
def varint(v):
    v &= (1 << 64) - 1
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)

def fv(field, v): return varint(field << 3) + varint(v)
def fb(field, b): return varint(field << 3 | 2) + varint(len(b)) + b

def parse(buf):
    i, out = 0, []
    while i < len(buf):
        key, i = rv(buf, i)
        f, w = key >> 3, key & 7
        if w == 0:
            v, i = rv(buf, i); out.append((f, v))
        elif w == 2:
            n, i = rv(buf, i); out.append((f, bytes(buf[i:i + n]))); i += n
        else:
            raise ValueError("wire type %d" % w)
    return out

def rv(buf, i):
    v = s = 0
    while True:
        b = buf[i]; i += 1
        v |= (b & 0x7F) << s; s += 7
        if not b & 0x80:
            return v, i

# ---------------------------------------------------------------- serial packet link
class Link:
    def __init__(self):
        self.s = serial.Serial(PORT, 115200, timeout=0.05)
        self.s.dtr = False; self.s.rts = True; time.sleep(0.1); self.s.rts = False  # reset the board
        self.buf = bytearray()
        self.log = bytearray()
        self.rx = bytearray()
        self.booted = False

    def pump(self):
        d = self.s.read(65536)
        if not d:
            return
        self.buf += d
        while True:
            k = self.buf.find(b"\xA5\x5A")
            if k < 0:
                self.text(self.buf[:-1] if self.buf.endswith(b"\xA5") else self.buf)
                self.buf = self.buf[-1:] if self.buf.endswith(b"\xA5") else bytearray()
                return
            if k:
                self.text(self.buf[:k])
                del self.buf[:k]
            if len(self.buf) < 4:
                return
            n = self.buf[2] << 8 | self.buf[3]
            if len(self.buf) < 4 + n:
                return
            self.rx += self.buf[4:4 + n]
            del self.buf[:4 + n]

    def text(self, b):
        self.log += b
        while b"\n" in self.log:
            line, _, self.log = self.log.partition(b"\n")
            t = line.decode(errors="replace").rstrip()
            if t and ("wifi:" not in t):
                print("   board| " + t, flush=True)
            if "stage boot -> ap" in t:
                self.booted = True

    def send(self, data):
        if not data:
            self.s.write(b"\xA5\x5A\x00\x00")
        for i in range(0, len(data), 4096):
            c = data[i:i + 4096]
            self.s.write(b"\xA5\x5A" + struct.pack(">H", len(c)) + c)

    def read(self, n, timeout=10):
        t0 = time.time()
        while len(self.rx) < n:
            self.pump()
            if time.time() - t0 > timeout:
                raise TimeoutError("waiting for %d bytes" % n)
        out = bytes(self.rx[:n]); del self.rx[:n]
        return out

# ---------------------------------------------------------------- the phone
class Phone:
    def __init__(self, link):
        self.l = link
        self.tls = None
        self.acks = 0
        self.max_unacked = 1
        self.unacked = 0
        self.opened = set()
        self.pings = 0

    def frame_out(self, ch, flags, payload, total=None):
        hdr = struct.pack(">BBH", ch, flags, len(payload))
        if total is not None:
            hdr += struct.pack(">I", total)
        self.l.send(hdr + payload)

    def enc(self, plain):
        self.tls.write(plain)
        return self.outgoing.read()

    def send(self, ch, msg_id, body=b"", control=False):
        plain = struct.pack(">H", msg_id) + body
        flags = F_FIRST | F_LAST | (F_CONTROL if control else 0)
        if self.tls:
            self.frame_out(ch, flags | F_ENC, self.enc(plain))
        else:
            self.frame_out(ch, flags, plain)

    def send_big(self, ch, msg_id, body):
        """Fragmented like a phone: 16 KB plaintext pieces, each encrypted on its own."""
        plain = struct.pack(">H", msg_id) + body
        if len(plain) <= 16384:
            return self.frame_out(ch, F_FIRST | F_LAST | F_ENC, self.enc(plain))
        pieces = [plain[i:i + 16384] for i in range(0, len(plain), 16384)]
        for k, p in enumerate(pieces):
            f = F_ENC | (F_FIRST if k == 0 else 0) | (F_LAST if k == len(pieces) - 1 else 0)
            self.frame_out(ch, f, self.enc(p), total=len(plain) if k == 0 else None)

    def recv(self, timeout=10):
        ch, flags, n = struct.unpack(">BBH", self.l.read(4, timeout))
        if flags & 3 == F_FIRST:
            self.l.read(4)
        payload = self.l.read(n)
        if flags & F_ENC:
            self.incoming.write(payload)
            payload = b""
            while True:
                try:
                    payload += self.tls.read(65536)
                except ssl.SSLWantReadError:
                    break
        assert flags & 3 == 3, "board fragments are not expected"
        return ch, flags, struct.unpack(">H", payload[:2])[0], payload[2:]

    def expect(self, ch, msg_id, what, timeout=10):
        while True:
            c, f, i, body = self.recv(timeout)
            if (c, i) == (ch, msg_id):
                return body
            self.handle(c, f, i, body, quiet=False)

    def handle(self, ch, flags, msg_id, body, quiet=True):
        if ch == 2 and msg_id == 0x8004:
            self.acks += 1
            self.unacked = max(0, self.unacked - 1)
        elif ch == 0 and msg_id == 12:
            self.pings += 1
        elif not quiet:
            print("phone: ch %d id 0x%04X %s" % (ch, msg_id, parse(body) if body else ""))

    def handshake(self):
        print("phone: waiting for version request")
        ch, flags, msg_id, body = self.recv(30)
        assert (ch, msg_id) == (0, 1), (ch, msg_id)
        major, minor = struct.unpack(">HH", body[:4])
        print("phone: version request %d.%d" % (major, minor))
        self.send(0, 2, struct.pack(">HHh", 1, minor, 0))

        d = tempfile.mkdtemp()
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", d + "/k.pem",
                        "-out", d + "/c.pem", "-days", "2", "-subj", "/CN=fake-phone"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(d + "/c.pem", d + "/k.pem")
        ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        self.incoming, self.outgoing = ssl.MemoryBIO(), ssl.MemoryBIO()
        tls = ctx.wrap_bio(self.incoming, self.outgoing, server_side=True)
        t0 = time.time()
        while True:
            try:
                tls.do_handshake()
                break
            except ssl.SSLWantReadError:
                out = self.outgoing.read()
                if out:
                    self.send(0, 3, out)
                ch, flags, msg_id, body = self.recv(20)
                assert msg_id == 3, "expected SSL data, got 0x%04X" % msg_id
                self.incoming.write(body)
        out = self.outgoing.read()
        if out:
            self.send(0, 3, out)
        print("phone: TLS done in %.1f s, %s" % (time.time() - t0, tls.cipher()[0]))
        ch, flags, msg_id, body = self.recv(10)
        assert (ch, msg_id) == (0, 4), "expected auth complete, got %d/0x%04X" % (ch, msg_id)
        print("phone: auth complete %s" % parse(body))
        self.tls = tls

    def discovery(self):
        self.send(0, 5, fb(4, b"Fake Pixel") + fb(5, b"Google"))
        body = self.expect(0, 6, "service discovery response")
        fields = parse(body)
        services = [parse(v) for f, v in fields if f == 1]
        print("phone: discovery response, %d bytes, services:" % len(body))
        for s in services:
            sid = dict(s)[1]
            kinds = {2: "sensors", 3: "media sink", 4: "input", 8: "navigation", 9: "playback"}
            kind = [kinds.get(f, str(f)) for f, _ in s if f != 1]
            extra = ""
            for f, v in s:
                if f == 3:
                    ms = parse(v)
                    vc = [parse(x) for g, x in ms if g == 4]
                    if vc:
                        extra = " video %s" % dict(vc[0])
            print("        channel %d: %s%s" % (sid, ", ".join(kind), extra))
        info = {f: v for f, v in fields if f != 1}
        print("        head unit: %s / driver position %s" % (info.get(8), info.get(6)))
        return [dict(s)[1] for s in services]

    def open_channels(self, chans):
        for c in chans:
            self.send(c, 7, fv(1, 0) + fv(2, c), control=True)
            body = self.expect(c, 8, "channel open")
            assert parse(body) == [(1, 0)], parse(body)
            self.opened.add(c)
        print("phone: opened channels %s" % sorted(self.opened))
        self.send(1, 0x8001, fv(1, 10) + fv(2, 0))           # NIGHT sensor
        self.expect(1, 0x8002, "sensor start")
        self.send(1, 0x8001, fv(1, 13) + fv(2, 0))           # DRIVING_STATUS
        self.expect(1, 0x8002, "sensor start")
        self.send(0, 11, fv(1, 123456789))                   # ping
        body = self.expect(0, 12, "ping response")
        assert parse(body) == [(1, 123456789)], parse(body)
        print("phone: sensors started, ping echoed")

    def video(self):
        self.send(2, 0x8000, fv(1, 3))
        cfg = parse(self.expect(2, 0x8003, "video config"))
        self.max_unacked = dict(cfg)[2]
        focus = parse(self.expect(2, 0x8008, "video focus"))
        print("phone: video config %s, focus %s" % (cfg, focus))
        self.send(2, 0x8001, fv(1, 7) + fv(2, 0))

        clip = open(CLIP, "rb").read()
        starts, k = [], 0
        while True:
            k = clip.find(b"\0\0\1", k)
            if k < 0:
                break
            starts.append(k - 1 if k > 0 and clip[k - 1] == 0 else k)
            k += 3
        nals = [clip[a:b] for a, b in zip(starts, starts[1:] + [len(clip)])]
        ntype = lambda n: n[n.index(b"\0\0\1") + 3] & 0x1F
        codec_cfg = next(n for n in nals if ntype(n) == 7) + next(n for n in nals if ntype(n) == 8)
        units, cur = [], b""
        for n in nals:
            if ntype(n) in (7, 8, 9):
                continue  # parameter sets travel as codec config, like a phone
            cur += n
            if ntype(n) in (1, 5):
                units.append(cur); cur = b""
        print("phone: %d access units, codec config %d bytes" % (len(units), len(codec_cfg)))
        self.send(2, 0x0001, codec_cfg)
        t0 = time.time(); sent = 0; k = 0; last_ping = t0; ts = 0
        while time.time() - t0 < SECONDS:
            if self.unacked < self.max_unacked:
                au = units[k % len(units)]; k += 1
                ts += 33333
                self.send_big(2, 0x0000, struct.pack(">Q", ts) + au)
                self.unacked += 1; sent += 1
            if time.time() - last_ping > 1:
                last_ping = time.time()
                self.send(0, 11, fv(1, int(last_ping * 1e6)))
            self.l.pump()
            while len(self.l.rx) >= 4:
                try:
                    self.handle(*self.recv(1))
                except TimeoutError:
                    break
            if self.unacked >= self.max_unacked:
                # wait briefly for an ACK
                try:
                    self.handle(*self.recv(2))
                except TimeoutError:
                    print("phone: no ACK for 2 s (sent %d, acks %d)" % (sent, self.acks))
                    self.unacked = 0
        dt = time.time() - t0
        print("phone: streamed %d frames in %.0f s -> %.1f fps acked, %d acks, %d ping replies"
              % (sent, dt, self.acks / dt, self.acks, self.pings))
        if GO_QUIET:
            print("phone: going quiet, waiting for the board to say bye-bye")
            t1 = time.time()
            while time.time() - t1 < 40:
                try:
                    ch, flags, msg_id, body = self.recv(1)
                except TimeoutError:
                    continue
                if (ch, msg_id) == (0, 15):
                    print("phone: board said bye-bye after %.0f s of silence %s" % (time.time() - t1, parse(body)))
                    self.send(0, 16)
                    return
            print("phone: board never said bye-bye")
            return
        self.send(0, 15, fv(1, 1))  # bye-bye
        try:
            self.expect(0, 16, "byebye response", timeout=3)
            print("phone: bye-bye answered")
        except TimeoutError:
            print("phone: no bye-bye response")

def main():
    link = Link()
    t = time.time()
    while not link.booted and time.time() - t < 10:
        link.pump()
    time.sleep(0.5)
    link.send(b"")  # hello: the board starts a session
    p = Phone(link)
    p.handshake()
    chans = p.discovery()
    p.open_channels(chans)
    p.video()
    t = time.time()
    while time.time() - t < 2:
        link.pump()

if __name__ == "__main__":
    main()
