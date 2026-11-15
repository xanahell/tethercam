#!/usr/bin/env python3
"""A fake usbmuxd over loopback TCP with a fake TetherCam iPhone behind it.

Exercises the same path a Windows receiver takes: usbmuxd as a TCP service
(Apple Mobile Device Service listens on 127.0.0.1:27015), reached through
USBMUXD_SOCKET_ADDRESS=HOST:PORT. It answers ListDevices (one USB device and
its Network twin, which a receiver must filter out) and Connect, and after a
successful Connect the same socket becomes the tunnel to the "phone": IUCM
HELLO, then CONFIG and VIDEO frames cut from a real Annex-B HEVC file.

Stdlib only. It checks the protocol rules a receiver must follow and prints a
line starting with VIOLATION on stderr for each one it sees broken.

  usage: fake_usbmuxd.py --hevc in.hevc [--width 1280] [--height 720] [--fps 30]
                         [--port 0] [--portfile FILE]
                         [--scenario ok|busy|no-client-info]

Scenarios: ok (default, protocol 1.2), no-client-info (announces 1.0 and
expects no CLIENT_INFO), busy (sends ERROR 1 BUSY right after Connect).
"""
import argparse
import plistlib
import re
import select
import socket
import struct
import sys
import threading
import time

MUX = struct.Struct("<IIII")  # length incl. header, version, message, tag
HDR = struct.Struct("<4sBBHI")
MAGIC = b"IUCM"

HELLO, START, STOP, CLIENT_INFO = 0x01, 0x02, 0x03, 0x04
CONFIG, VIDEO = 0x10, 0x11
PING, PONG, ERROR = 0x20, 0x21, 0x30

USB_DEVICE_ID = 7
NET_DEVICE_ID = 8
SERIAL = "00008130-0012345678901234"
PHONE_PORT = 7878


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def violation(msg):
    log("VIOLATION %s" % msg)


# ---------------------------------------------------------------- HEVC input

def split_annexb(data):
    """NAL units of an Annex-B stream, start codes removed."""
    starts = [m.end() for m in re.finditer(b"\x00\x00\x01", data)]
    nals = []
    for i, s in enumerate(starts):
        end = starts[i + 1] - 3 if i + 1 < len(starts) else len(data)
        nals.append(data[s:end].rstrip(b"\x00"))
    return nals


def nal_type(nal):
    return (nal[0] >> 1) & 0x3F


def unescape(rbsp):
    """Drop emulation prevention bytes (00 00 03 -> 00 00)."""
    out = bytearray()
    zeros = 0
    for b in rbsp:
        if zeros >= 2 and b == 3:
            zeros = 0
            continue
        out.append(b)
        zeros = zeros + 1 if b == 0 else 0
    return bytes(out)


def build_hvcc(vps, sps, pps):
    """hvcC record (ISO/IEC 14496-15) from the three parameter sets."""
    ptl = unescape(sps[2:])  # skip the 2-byte NAL header
    # ptl[0]: vps id, sub layers, nesting; ptl[1]: space/tier/profile;
    # ptl[2:6]: compatibility flags; ptl[6:12]: constraint flags; ptl[12]: level
    rec = bytearray()
    rec.append(1)                      # configurationVersion
    rec.append(ptl[1])                 # profile space, tier, profile idc
    rec += ptl[2:6]
    rec += ptl[6:12]
    rec.append(ptl[12])                # level idc
    rec += struct.pack(">H", 0xF000)   # min spatial segmentation
    rec.append(0xFC)                   # parallelism type
    rec.append(0xFD)                   # chroma format 4:2:0
    rec.append(0xF8)                   # luma bit depth 8
    rec.append(0xF8)                   # chroma bit depth 8
    rec += struct.pack(">H", 0)        # average frame rate
    rec.append(0x0F)                   # 1 temporal layer, nested, 4-byte lengths
    rec.append(3)                      # three arrays
    for typ, nal in ((32, vps), (33, sps), (34, pps)):
        rec.append(0x80 | typ)
        rec += struct.pack(">HH", 1, len(nal))
        rec += nal
    return bytes(rec)


def load_stream(path):
    """-> (hvcc, [(keyframe, [nal, ...]), ...]) with parameter sets pulled out
    of the frames, the way the iPhone app sends them (hvcC only)."""
    with open(path, "rb") as f:
        nals = split_annexb(f.read())
    params, frames, pending = {}, [], []
    for nal in nals:
        t = nal_type(nal)
        if t in (32, 33, 34):
            params.setdefault(t, nal)
        elif t < 32:
            frames.append((16 <= t <= 21, pending + [nal]))
            pending = []
        elif t == 39:  # prefix SEI rides in front of the next VCL NAL
            pending.append(nal)
        # AUD, EOS, suffix SEI: dropped
    if len(params) != 3:
        raise SystemExit("input needs VPS, SPS and PPS (encode with repeat-headers=1)")
    if not frames or not frames[0][0]:
        raise SystemExit("input must start with a keyframe")
    return build_hvcc(params[32], params[33], params[34]), frames


# -------------------------------------------------------------------- IUCM

def frame(msg_type, payload=b"", flags=0):
    return HDR.pack(MAGIC, msg_type, flags, 0, len(payload)) + payload


def s8(text):
    raw = text.encode("utf-8")
    return bytes([len(raw)]) + raw


def hello_payload(version):
    cams = [(0, 0, "Back Wide"), (1, 0, "Back Ultra Wide"), (2, 1, "Front")]
    out = struct.pack("<H", version) + s8("Fake iPhone") + s8("0.4.0") + bytes([len(cams)])
    for cid, pos, name in cams:
        out += bytes([cid, pos]) + s8(name)
    return out


class Reader:
    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        self.buf += data

    def messages(self):
        while len(self.buf) >= HDR.size:
            magic, mtype, flags, reserved, length = HDR.unpack_from(self.buf, 0)
            if magic != MAGIC:
                raise ValueError("bad magic %r" % magic)
            if reserved != 0:
                violation("reserved field is %d, must be 0" % reserved)
            if len(self.buf) < HDR.size + length:
                return
            payload = bytes(self.buf[HDR.size:HDR.size + length])
            del self.buf[:HDR.size + length]
            yield mtype, flags, payload


def video_payload(pts_us, nals):
    out = struct.pack("<Q", pts_us)
    for nal in nals:
        out += struct.pack(">I", len(nal)) + nal
    return out


def run_phone(conn, cfg, hvcc, frames):
    """The IUCM server side. Returns when the receiver is done."""
    if cfg.scenario == "busy":
        text = b"busy"
        conn.sendall(frame(ERROR, struct.pack("<HH", 1, len(text)) + text))
        return
    version = 0x0100 if cfg.scenario == "no-client-info" else 0x0102
    conn.sendall(frame(HELLO, hello_payload(version)))

    reader = Reader()
    streaming = False
    got_client_info = False
    idx, pts, next_t = 0, 0, 0.0
    dt = 1.0 / cfg.fps
    sent = 0

    while True:
        timeout = max(0.0, next_t - time.monotonic()) if streaming else 1.0
        ready, _, _ = select.select([conn], [], [], timeout)
        if ready:
            data = conn.recv(65536)
            if not data:
                log("phone: receiver closed the tunnel after %d frames" % sent)
                return
            reader.feed(data)
            for mtype, _flags, payload in reader.messages():
                if mtype == CLIENT_INFO:
                    got_client_info = True
                    if version < 0x0102:
                        violation("CLIENT_INFO sent to a 1.0 app")
                    kind, nlen = payload[0], payload[1]
                    log("phone: CLIENT_INFO kind=%d name=%r"
                        % (kind, payload[2:2 + nlen].decode("utf-8")))
                elif mtype == START:
                    if len(payload) not in (11, 12):
                        violation("START length %d, expected 11 or 12" % len(payload))
                    cam, w, h, fps, kbps = struct.unpack_from("<BHHHI", payload, 0)
                    sflags = payload[11] if len(payload) == 12 else 0
                    log("phone: START camera=%d %dx%d@%d %dkbps len=%d flags=%d"
                        % (cam, w, h, fps, kbps, len(payload), sflags))
                    if version >= 0x0102 and not got_client_info:
                        log("phone: note: START without CLIENT_INFO (optional)")
                    if sflags & 1:
                        violation("receiver asked for audio, which this tool never does")
                    conn.sendall(frame(CONFIG, struct.pack("<HHHI", cfg.width, cfg.height,
                                                           cfg.fps, len(hvcc)) + hvcc))
                    streaming = True
                    next_t = time.monotonic()
                elif mtype == STOP:
                    log("phone: STOP after %d frames" % sent)
                    return
                elif mtype == PING:
                    if len(payload) != 8:
                        violation("PING payload is %d bytes, expected 8" % len(payload))
                    conn.sendall(frame(PONG, payload))
                else:
                    violation("unexpected message type 0x%02x from the receiver" % mtype)
        if streaming and time.monotonic() >= next_t:
            keyframe, nals = frames[idx % len(frames)]
            conn.sendall(frame(VIDEO, video_payload(pts, nals), 1 if keyframe else 0))
            sent += 1
            idx += 1
            pts += int(dt * 1e6)
            next_t += dt


# ------------------------------------------------------------------ usbmuxd

def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


def send_mux(conn, tag, body):
    raw = plistlib.dumps(body)
    conn.sendall(MUX.pack(MUX.size + len(raw), 1, 8, tag) + raw)


def device_list():
    return {"DeviceList": [
        {"DeviceID": USB_DEVICE_ID, "MessageType": "Attached", "Properties": {
            "ConnectionType": "USB", "DeviceID": USB_DEVICE_ID, "LocationID": 1048576,
            "ProductID": 4776, "SerialNumber": SERIAL}},
        {"DeviceID": NET_DEVICE_ID, "MessageType": "Attached", "Properties": {
            "ConnectionType": "Network", "DeviceID": NET_DEVICE_ID,
            "EscapedFullServiceName": "fake._apple-mobdev2._tcp.local.",
            "SerialNumber": SERIAL}},
    ]}


def handle(conn, cfg, hvcc, frames):
    try:
        while True:
            try:
                length, version, message, tag = MUX.unpack(recv_exact(conn, MUX.size))
            except EOFError:
                return
            if version != 1 or message != 8:
                violation("usbmux header version=%d message=%d" % (version, message))
                return
            req = plistlib.loads(recv_exact(conn, length - MUX.size))
            kind = req.get("MessageType")
            if kind == "ListDevices":
                send_mux(conn, tag, device_list())
            elif kind == "Connect":
                want = socket.htons(PHONE_PORT)  # the plist carries the port byte-swapped
                ok = req.get("DeviceID") == USB_DEVICE_ID and req.get("PortNumber") == want
                if req.get("PortNumber") == PHONE_PORT:
                    violation("Connect carried the port in host order")
                send_mux(conn, tag, {"MessageType": "Result", "Number": 0 if ok else 3})
                if ok:
                    run_phone(conn, cfg, hvcc, frames)
                return
            else:
                send_mux(conn, tag, {"MessageType": "Result", "Number": 1})
    except (ConnectionError, ValueError) as exc:
        log("session ended: %s" % exc)
    finally:
        conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hevc", required=True, help="Annex-B HEVC with repeat-headers")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--port", type=int, default=0, help="0 picks a free port")
    ap.add_argument("--portfile", help="write the chosen port here once listening")
    ap.add_argument("--scenario", choices=("ok", "busy", "no-client-info"), default="ok")
    cfg = ap.parse_args()

    hvcc, frames = load_stream(cfg.hevc)
    log("loaded %d frames, hvcC %d bytes" % (len(frames), len(hvcc)))

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", cfg.port))
    srv.listen(8)
    port = srv.getsockname()[1]
    if cfg.portfile:
        with open(cfg.portfile, "w") as f:
            f.write("%d\n" % port)
    log("fake usbmuxd on 127.0.0.1:%d (scenario %s)" % (port, cfg.scenario))

    while True:
        conn, _ = srv.accept()
        threading.Thread(target=handle, args=(conn, cfg, hvcc, frames), daemon=True).start()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
