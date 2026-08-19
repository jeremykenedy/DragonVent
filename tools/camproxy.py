#!/usr/bin/env python3
"""
camproxy - re-serve a Bambu LAN camera stream as MJPEG over plain HTTP.

Why this exists
---------------
The printer's camera lives behind a raw TLS socket on port 6000 speaking a
length-prefixed frame protocol. A browser cannot open that, so nothing can
embed it directly. This holds the socket, pulls JPEG frames, and re-serves
them as multipart/x-mixed-replace, which an <img> tag renders natively.

It deliberately does NOT run on the vent. The ESP32 is already carrying WiFi,
TLS, an HTTP server and a real-time motor task in ~200 KB of heap, and a single
frame here is 50-100 KB. Putting the motor task behind a network buffer to save
a Raspberry-Pi-sized process is a bad trade.

    printer:6000 --(TLS)--> camproxy --(MJPEG/HTTP)--> DragonVent Video tab

Run:
    python3 camproxy.py
Then point the vent's Video tab at  http://<this-host>:8766/stream.mjpeg

Endpoints:
    /stream.mjpeg   multipart MJPEG, what the <img> points at
    /snapshot.jpg   single most recent frame
    /status         JSON health, so the UI can say WHY it is dark
"""
import http.server
import json
import os
import socket
import socketserver
import ssl
import struct
import threading
import time

# Credentials come from the environment, never from this file. It lives in a
# public repo; a printer access code committed here would be a secret in git
# history forever, and rotating it means re-pairing every client you own.
PRINTER_IP  = os.environ.get("PRINTER_IP",  "")
ACCESS_CODE = os.environ.get("ACCESS_CODE", "")
USERNAME    = os.environ.get("CAM_USER",    "bblp")   # fixed for all Bambu printers
CAM_PORT    = int(os.environ.get("CAM_PORT", "6000"))
PORT        = int(os.environ.get("PROXY_PORT", "8766"))

# The printer answers a refusal as a 16-byte header + a 4-byte payload of
# 0xFFFFFFFF. Seen on a P2S in cloud mode: liveview is gated behind the
# printer's LAN settings, and the refusal is identical whether the printer is
# idle or printing, and whether user/code are in the documented order or
# swapped. So treat it as policy, not as a handshake bug, and say so.
REFUSAL = 0xFFFFFFFF

STATE = {
    "connected": False,
    "frames": 0,
    "fps": 0.0,
    "last_frame_at": 0.0,
    "last_error": "not started",
    "refused": False,
    "printer": f"{PRINTER_IP}:{CAM_PORT}",
}
_lock = threading.Lock()
_frame = [None]          # latest JPEG bytes
_frame_seq = [0]
_cond = threading.Condition()


def auth_blob():
    b = struct.pack("<IIII", 0x40, 0x3000, 0, 0)
    b += USERNAME.encode().ljust(32, b"\x00")
    b += ACCESS_CODE.encode().ljust(32, b"\x00")
    return b


def set_err(msg, refused=False):
    with _lock:
        STATE["connected"] = False
        STATE["last_error"] = msg
        STATE["refused"] = refused
    print(f"[camproxy] {msg}", flush=True)


def reader():
    """Hold the camera socket, publish frames. Reconnects forever with backoff."""
    backoff = 2
    while True:
        try:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            try:
                ctx.set_ciphers("DEFAULT:@SECLEVEL=0")
            except ssl.SSLError:
                pass

            raw = socket.create_connection((PRINTER_IP, CAM_PORT), timeout=8)
            s = ctx.wrap_socket(raw, server_hostname=PRINTER_IP)
            s.sendall(auth_blob())
            s.settimeout(15)

            buf = b""
            t_win, n_win = time.time(), 0
            got_any = False

            while True:
                chunk = s.recv(65536)
                if not chunk:
                    raise ConnectionError("printer closed the connection")
                buf += chunk

                while len(buf) >= 16:
                    n = struct.unpack("<I", buf[:4])[0]
                    if n == 0 or n > 8_000_000:
                        raise ValueError(f"bad frame length {n}")
                    if len(buf) < 16 + n:
                        break
                    payload = buf[16:16 + n]
                    buf = buf[16 + n:]

                    # A 4-byte 0xFFFFFFFF body is the refusal, not a picture.
                    if n == 4 and struct.unpack("<I", payload)[0] == REFUSAL:
                        set_err(
                            "printer refused the camera stream (-1). Enable LAN Mode "
                            "Liveview on the printer; on current firmware that generally "
                            "means switching it to LAN Only Mode.",
                            refused=True)
                        raise ConnectionError("refused")
                    if n == 8 and payload[:4] == b"\xff\xff\xff\xff":
                        set_err("printer refused the camera stream (-1). "
                                "Enable LAN Mode Liveview on the printer.", refused=True)
                        raise ConnectionError("refused")

                    if payload[:2] != b"\xff\xd8":
                        continue          # not a JPEG SOI, skip it

                    with _cond:
                        _frame[0] = payload
                        _frame_seq[0] += 1
                        _cond.notify_all()

                    n_win += 1
                    got_any = True
                    now = time.time()
                    with _lock:
                        STATE["frames"] += 1
                        STATE["last_frame_at"] = now
                        STATE["connected"] = True
                        STATE["last_error"] = ""
                        STATE["refused"] = False
                        if now - t_win >= 2.0:
                            STATE["fps"] = round(n_win / (now - t_win), 1)
                            t_win, n_win = now, 0
                    backoff = 2

            # not reached
        except Exception as e:
            with _lock:
                refused = STATE["refused"]
            if not refused:
                set_err(f"{type(e).__name__}: {e}")
            time.sleep(backoff)
            backoff = min(backoff * 2, 30)


def snapshot(timeout=5.0):
    with _cond:
        if _frame[0] is None:
            _cond.wait(timeout)
        return _frame[0]


class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _cors(self):
        # The SPA is served from the vent, this from another host, so /status
        # is a cross-origin fetch. <img> does not need this; the fetch does.
        self.send_header("Access-Control-Allow-Origin", "*")

    def do_OPTIONS(self):
        self.send_response(204)
        self._cors()
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        p = self.path.split("?")[0]
        if p == "/status":
            with _lock:
                st = dict(STATE)
            st["stale_s"] = round(time.time() - st["last_frame_at"], 1) if st["last_frame_at"] else None
            body = json.dumps(st).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self._cors()
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if p == "/snapshot.jpg":
            f = snapshot()
            if not f:
                self.send_error(503, "no frame")
                return
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self._cors()
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(f)))
            self.end_headers()
            self.wfile.write(f)
            return

        if p in ("/stream.mjpeg", "/", "/stream"):
            self.send_response(200)
            self.send_header("Content-Type",
                             "multipart/x-mixed-replace; boundary=frameboundary")
            self._cors()
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            last = -1
            try:
                while True:
                    with _cond:
                        if _frame_seq[0] == last:
                            _cond.wait(10)
                        f, last = _frame[0], _frame_seq[0]
                    if not f:
                        continue
                    self.wfile.write(b"--frameboundary\r\n")
                    self.wfile.write(b"Content-Type: image/jpeg\r\n")
                    self.wfile.write(b"Content-Length: " + str(len(f)).encode() + b"\r\n\r\n")
                    self.wfile.write(f)
                    self.wfile.write(b"\r\n")
            except (BrokenPipeError, ConnectionResetError):
                pass
            return

        self.send_error(404)


class TS(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    missing = [n for n, v in (("PRINTER_IP", PRINTER_IP), ("ACCESS_CODE", ACCESS_CODE)) if not v]
    if missing:
        raise SystemExit(
            "camproxy: set " + " and ".join(missing) + " in the environment.\n"
            "  PRINTER_IP   your printer's LAN address\n"
            "  ACCESS_CODE  the LAN access code from the printer's screen\n\n"
            "  PRINTER_IP=192.168.1.x ACCESS_CODE=xxxxxxxx python3 camproxy.py")
    threading.Thread(target=reader, daemon=True).start()
    ip = socket.gethostbyname(socket.gethostname())
    print(f"[camproxy] printer {PRINTER_IP}:{CAM_PORT}", flush=True)
    print(f"[camproxy] stream  http://{ip}:{PORT}/stream.mjpeg", flush=True)
    print(f"[camproxy] status  http://{ip}:{PORT}/status", flush=True)
    TS(("0.0.0.0", PORT), H).serve_forever()
