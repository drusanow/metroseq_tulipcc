"""A stand-in for the Tulip launcher's web server, for developing and testing
the web app (launcher/main/web/index.html) and tools/tulip.py on a PC.

It implements the same API as launcher/main/web.c, with the same rules:
PIN header, 5-wrong-PINs lockout, paths confined to the files root,
uploads written to a temp file then renamed, apps recognised by their
ESP32-S3 image header.

    python3 tests/web_mock.py [--root DIR] [--port 8080] [--pin 1234]
"""
import argparse
import json
import os
import shutil
import struct
import sys
import tempfile
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
INDEX = os.path.join(HERE, "..", "launcher", "main", "web", "index.html")
BAD_CHARS = set('/\\:*?"<>|')


def app_info(path):
    """Mirror of core/appimg.c: (name, version) or None."""
    try:
        with open(path, "rb") as f:
            b = f.read(288)
    except OSError:
        return None
    if len(b) < 32 + 176 or b[0] != 0xE9 or not (0 < b[1] <= 16):
        return None
    if struct.unpack_from("<H", b, 12)[0] != 9:
        return None
    d = b[32:]
    if struct.unpack_from("<I", d, 0)[0] != 0xABCD5432:
        return None
    cstr = lambda raw: raw.split(b"\0", 1)[0].decode("utf-8", "replace")
    name, ver = cstr(d[48:80]), cstr(d[16:48])
    return (name, ver) if name else None


def valid_name(n):
    return (0 < len(n) <= 64 and n not in (".", "..") and not n.endswith((" ", "."))
            and not any(c in BAD_CHARS or ord(c) < 32 for c in n))


class Device:
    def __init__(self, root, pin):
        self.root = os.path.realpath(root)
        self.pin = pin
        self.fails = 0
        self.locked_until = 0.0
        self.launched = []
        os.makedirs(os.path.join(self.root, "apps"), exist_ok=True)

    def resolve(self, rel):
        """Mirror of path_resolve(): absolute path inside root, or None."""
        segs = []
        for s in rel.split("/"):
            if s in ("", "."):
                continue
            if s == "..":
                if not segs:
                    return None
                segs.pop()
                continue
            if any(ord(c) < 32 or c == "\\" for c in s):
                return None
            segs.append(s)
        return os.path.join(self.root, *segs)

    def rel(self, abs_path):
        r = os.path.relpath(abs_path, self.root)
        return "/" if r == "." else "/" + r.replace(os.sep, "/")


class Handler(BaseHTTPRequestHandler):
    dev: Device = None

    def log_message(self, *a):
        pass

    def send_json(self, status, obj):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def err(self, status, msg):
        self.send_json(status, {"ok": False, "error": msg})

    def auth(self):
        d = self.dev
        now = time.monotonic()
        if now < d.locked_until:
            self.err(429, "too many wrong PINs - wait 30 s")
            return False
        if self.headers.get("X-Tulip-Pin") == d.pin:
            d.fails = 0
            return True
        d.fails += 1
        if d.fails >= 5:
            d.fails = 0
            d.locked_until = now + 30
        self.err(401, "wrong or missing PIN")
        return False

    def q(self, key):
        qs = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        if key not in qs:
            return None
        return self.dev.resolve(qs[key][0])

    def route(self):
        return urllib.parse.urlparse(self.path).path

    def do_GET(self):
        r = self.route()
        if r == "/":
            body = open(INDEX, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if not r.startswith("/api/"):
            return self.err(404, "not found")
        if not self.auth():
            return
        d = self.dev
        if r == "/api/info":
            apps = []
            appdir = os.path.join(d.root, "apps")
            for dirpath, dirs, files in os.walk(appdir):
                depth = os.path.relpath(dirpath, appdir).count(os.sep) + (dirpath != appdir)
                if depth > 1:
                    continue
                for f in files:
                    p = os.path.join(dirpath, f)
                    if f.startswith(".") or not f.lower().endswith(".bin"):
                        continue
                    info = app_info(p)
                    if info:
                        apps.append({"path": d.rel(p), "name": info[0], "version": info[1],
                                     "size": os.path.getsize(p), "ready": False})
            apps.sort(key=lambda a: a["name"].lower())
            st = shutil.disk_usage(d.root)
            total = 16 * 1024 * 1024
            used = sum(os.path.getsize(os.path.join(a, f)) for a, _, fs in os.walk(d.root) for f in fs)
            return self.send_json(200, {"ok": True, "device": "Tulip CC", "launcher": "mock",
                                        "ip": "127.0.0.1", "total": total,
                                        "free": max(0, total - used), "apps": apps})
        if r == "/api/ls":
            p = self.q("path")
            if p is None:
                return self.err(400, "bad path")
            if not os.path.isdir(p):
                return self.err(404, "no such folder")
            ents = []
            for n in os.listdir(p):
                if n.startswith("."):
                    continue
                fp = os.path.join(p, n)
                ents.append({"name": n, "dir": os.path.isdir(fp),
                             "size": 0 if os.path.isdir(fp) else os.path.getsize(fp)})
            ents.sort(key=lambda e: (not e["dir"], e["name"].lower()))
            return self.send_json(200, {"ok": True, "path": d.rel(p), "entries": ents})
        if r == "/api/file":
            p = self.q("path")
            if p is None or os.path.isdir(p):
                return self.err(400, "bad path")
            if not os.path.exists(p):
                return self.err(404, "no such file")
            body = open(p, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", 'attachment; filename="%s"' % os.path.basename(p))
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.err(404, "not found")

    def do_PUT(self):
        if self.route() != "/api/file":
            return self.err(404, "not found")
        if not self.auth():
            return
        p = self.q("path")
        n = int(self.headers.get("Content-Length", "0"))
        if p is None or not valid_name(os.path.basename(p)):
            self.rfile.read(n)
            return self.err(400, "bad file name")
        parent = os.path.dirname(p)
        if not os.path.isdir(parent):
            self.rfile.read(n)
            return self.err(404, "folder does not exist")
        if os.path.isdir(p):
            self.rfile.read(n)
            return self.err(409, "a folder has that name")
        tmp = os.path.join(parent, ".upload.tmp")
        with open(tmp, "wb") as f:
            left = n
            while left:
                chunk = self.rfile.read(min(65536, left))
                if not chunk:
                    break
                f.write(chunk)
                left -= len(chunk)
        os.replace(tmp, p)
        out = {"ok": True, "size": n}
        info = app_info(p) if p.lower().endswith(".bin") else None
        if info:
            out["app"] = {"name": info[0], "version": info[1]}
        self.send_json(200, out)

    def do_POST(self):
        r = self.route()
        if not self.auth():
            return
        d = self.dev
        if r == "/api/mkdir":
            p = self.q("path")
            if p is None or not valid_name(os.path.basename(p)):
                return self.err(400, "bad folder name")
            if os.path.exists(p):
                return self.err(409, "already exists")
            os.mkdir(p)
            return self.send_json(200, {"ok": True})
        if r == "/api/rm":
            p = self.q("path")
            if p is None:
                return self.err(400, "bad path")
            if p in (d.root, os.path.join(d.root, "apps")):
                return self.err(400, "that folder can't be deleted")
            if not os.path.exists(p):
                return self.err(404, "no such file")
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
            return self.send_json(200, {"ok": True})
        if r == "/api/mv":
            a, b = self.q("from"), self.q("to")
            if a is None or b is None or not valid_name(os.path.basename(b)):
                return self.err(400, "bad path")
            if a == os.path.join(d.root, "apps"):
                return self.err(400, "the apps folder stays put")
            if not os.path.exists(a):
                return self.err(404, "no such file")
            if os.path.isdir(b):
                return self.err(409, "a folder has that name")
            os.replace(a, b)
            return self.send_json(200, {"ok": True})
        if r == "/api/launch":
            p = self.q("path")
            if p is None or not app_info(p):
                return self.err(400, "not an app, or busy")
            d.launched.append(d.rel(p))
            return self.send_json(200, {"ok": True, "message": "launching - the Tulip will restart"})
        self.err(404, "not found")


def serve(root, port, pin):
    dev = Device(root, pin)
    handler = type("H", (Handler,), {"dev": dev})
    srv = ThreadingHTTPServer(("127.0.0.1", port), handler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    return srv, dev


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=None)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--pin", default="1234")
    a = ap.parse_args()
    root = a.root or tempfile.mkdtemp(prefix="tulip-files-")
    srv, _ = serve(root, a.port, a.pin)
    print("mock Tulip at http://127.0.0.1:%d  (PIN %s, files in %s)" % (a.port, a.pin, root))
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        sys.exit(0)
