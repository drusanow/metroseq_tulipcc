#!/usr/bin/env python3
"""tulip.py - move apps and files to a Tulip CC running the launcher, over Wi-Fi.

Standard library only. Talks to the launcher's web API (launcher/main/web.h).

    tulip.py [--host tulip.local] [--pin 1234] COMMAND ...

    info                         device, storage, apps
    ls [PATH]                    list a folder            (default /apps)
    push FILE... [--to DIR] [--launch]
                                 upload (.bin apps go to /apps, .elf apps
                                 and their .json to /studio/apps);
                                 --launch runs the (last) app afterwards
    pull PATH [LOCAL]            download
    mkdir PATH | rm PATH | mv FROM TO
    launch PATH                  install + run an app (the Tulip restarts into it)

The PIN is on the Tulip's WI-FI page. Set TULIP_HOST / TULIP_PIN to skip the flags.
Typical app loop:  idf.py build && tulip.py push build/myapp.bin --launch
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request


class Tulip:
    def __init__(self, host, pin, timeout=60):
        self.base = host if host.startswith("http") else "http://" + host
        self.pin = pin
        self.timeout = timeout

    def call(self, method, path, params=None, data=None, raw=False):
        url = self.base + path + ("?" + urllib.parse.urlencode(params) if params else "")
        req = urllib.request.Request(url, data=data, method=method, headers={"X-Tulip-Pin": self.pin})
        if data is not None:
            req.add_header("Content-Type", "application/octet-stream")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                body = r.read()
        except urllib.error.HTTPError as e:
            body = e.read()
            try:
                msg = json.loads(body).get("error", body.decode())
            except ValueError:
                msg = body.decode(errors="replace")
            raise SystemExit("tulip: %s (HTTP %d)" % (msg, e.code))
        except urllib.error.URLError as e:
            raise SystemExit("tulip: can't reach %s (%s) - is Wi-Fi on and the address right?"
                             % (self.base, e.reason))
        return body if raw else json.loads(body)


def remote(dirpath, name):
    return (dirpath.rstrip("/") or "") + "/" + name


def default_dir(name, files):
    """.bin apps -> /apps; loadable Studio apps (.elf, and the .json pushed
    with one) -> /studio/apps; anything else -> /."""
    low = name.lower()
    if low.endswith(".bin"):
        return "/apps"
    stem = os.path.splitext(name)[0]
    if low.endswith(".elf") or (low.endswith(".json") and
                                any(os.path.basename(f) == stem + ".elf" for f in files)):
        return "/studio/apps"
    return "/"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=os.environ.get("TULIP_HOST", "tulip.local"))
    ap.add_argument("--pin", default=os.environ.get("TULIP_PIN", ""))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info")
    p = sub.add_parser("ls")
    p.add_argument("path", nargs="?", default="/apps")
    p = sub.add_parser("push")
    p.add_argument("files", nargs="+")
    p.add_argument("--to", default=None, help="remote folder (default /apps for .bin, else /)")
    p.add_argument("--launch", action="store_true")
    p = sub.add_parser("pull")
    p.add_argument("path")
    p.add_argument("local", nargs="?")
    for c in ("mkdir", "rm", "launch"):
        sub.add_parser(c).add_argument("path")
    p = sub.add_parser("mv")
    p.add_argument("src")
    p.add_argument("dst")
    a = ap.parse_args(argv)
    if not a.pin:
        ap.error("the PIN is required (--pin or TULIP_PIN); it's on the Tulip's WI-FI page")
    t = Tulip(a.host, a.pin)

    if a.cmd == "info":
        i = t.call("GET", "/api/info")
        print("%s  launcher %s  %s" % (i["device"], i["launcher"], i["ip"]))
        print("storage: %.1f MB free of %.1f MB" % (i["free"] / 1048576, i["total"] / 1048576))
        for app in i["apps"]:
            print("  %-20s %-10s %8d  %s%s" % (app["name"], app["version"], app["size"], app["path"],
                                              "  [ready]" if app["ready"] else ""))
    elif a.cmd == "ls":
        r = t.call("GET", "/api/ls", {"path": a.path})
        print(r["path"])
        for e in r["entries"]:
            print("  %s/" % e["name"] if e["dir"] else "  %-40s %10d" % (e["name"], e["size"]))
    elif a.cmd == "push":
        last_app = None
        for f in a.files:
            name = os.path.basename(f)
            dest_dir = a.to or default_dir(name, a.files)
            with open(f, "rb") as fh:
                data = fh.read()
            dest = remote(dest_dir, name)
            r = t.call("PUT", "/api/file", {"path": dest}, data=data)
            if "app" in r:
                last_app = dest
                print("uploaded %s -> %s  (app %s v%s)" % (f, dest, r["app"]["name"], r["app"]["version"]))
            else:
                print("uploaded %s -> %s  (%d bytes)" % (f, dest, r["size"]))
        if a.launch:
            if not last_app:
                raise SystemExit("tulip: --launch, but none of those files is an app")
            t.call("POST", "/api/launch", {"path": last_app})
            print("launching %s - the Tulip is restarting into it" % last_app)
    elif a.cmd == "pull":
        data = t.call("GET", "/api/file", {"path": a.path}, raw=True)
        out = a.local or os.path.basename(a.path)
        with open(out, "wb") as fh:
            fh.write(data)
        print("saved %s (%d bytes)" % (out, len(data)))
    elif a.cmd == "mkdir":
        t.call("POST", "/api/mkdir", {"path": a.path})
        print("created %s" % a.path)
    elif a.cmd == "rm":
        t.call("POST", "/api/rm", {"path": a.path})
        print("deleted %s" % a.path)
    elif a.cmd == "mv":
        t.call("POST", "/api/mv", {"from": a.src, "to": a.dst})
        print("moved %s -> %s" % (a.src, a.dst))
    elif a.cmd == "launch":
        t.call("POST", "/api/launch", {"path": a.path})
        print("launching %s - the Tulip is restarting into it" % a.path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
