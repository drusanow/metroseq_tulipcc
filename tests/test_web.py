"""End-to-end test of the transfer path: tools/tulip.py and the real web app
(launcher/main/web/index.html, in headless Chromium) against web_mock.py,
which implements the launcher's API rules.

    python3 tests/test_web.py      (needs Playwright + its Chromium)
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import web_mock  # noqa: E402

PIN = "4821"
failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print("  FAIL:", msg)


def fake_app(path, name, version):
    b = bytearray(1024)
    b[0], b[1] = 0xE9, 3
    struct.pack_into("<H", b, 12, 9)
    struct.pack_into("<I", b, 32, 0xABCD5432)
    b[32 + 16:32 + 16 + len(version)] = version.encode()
    b[32 + 48:32 + 48 + len(name)] = name.encode()
    with open(path, "wb") as f:
        f.write(b)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def cli(port, *args, pin=PIN):
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "tulip.py"),
                        "--host", "127.0.0.1:%d" % port, "--pin", pin, *args],
                       capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def test_cli(port, dev, work):
    app = os.path.join(work, "blinky.bin")
    fake_app(app, "blinky", "0.9.1")
    rc, out = cli(port, "push", app, "--launch")
    check(rc == 0 and "app blinky v0.9.1" in out and "launching /apps/blinky.bin" in out, "push --launch: " + out)
    check(dev.launched == ["/apps/blinky.bin"], "mock saw the launch: %r" % dev.launched)
    rc, out = cli(port, "info")
    check(rc == 0 and "blinky" in out and "0.9.1" in out, "info lists the app: " + out)
    rc, out = cli(port, "mkdir", "/samples")
    check(rc == 0, "mkdir")
    txt = os.path.join(work, "notes.txt")
    open(txt, "w").write("hello tulip\n")
    rc, out = cli(port, "push", txt, "--to", "/samples")
    check(rc == 0 and os.path.exists(os.path.join(dev.root, "samples", "notes.txt")), "push to folder")
    rc, out = cli(port, "mv", "/samples/notes.txt", "/samples/readme.txt")
    check(rc == 0, "mv")
    os.chdir(work)
    rc, out = cli(port, "pull", "/samples/readme.txt", "back.txt")
    check(rc == 0 and open("back.txt").read() == "hello tulip\n", "pull round-trips")
    rc, out = cli(port, "ls", "/samples")
    check(rc == 0 and "readme.txt" in out, "ls")
    rc, out = cli(port, "rm", "/samples")
    check(rc == 0 and not os.path.exists(os.path.join(dev.root, "samples")), "rm folder")
    # safety
    rc, out = cli(port, "ls", "/", pin="0000")
    check(rc != 0 and "wrong or missing PIN" in out, "wrong PIN refused: " + out)
    rc, out = cli(port, "rm", "/../etc")
    check(rc != 0 and "bad path" in out, "path escape refused: " + out)
    rc, out = cli(port, "rm", "/apps")
    check(rc != 0, "apps folder can't be deleted")
    print("cli: push/--launch, info, mkdir, push --to, mv, pull, ls, rm; wrong PIN + escapes refused")


def test_browser(port, dev, work):
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("browser: skipped (no Playwright)")
        return
    shot_dir = os.path.join(HERE, "build")
    os.makedirs(shot_dir, exist_ok=True)
    url = "http://127.0.0.1:%d/" % port
    with sync_playwright() as pw:
        try:
            b = pw.chromium.launch()
        except Exception:
            # fall back to any Chromium already on the machine
            import glob
            exe = sorted(glob.glob("/opt/pw-browsers/chromium-*/chrome-linux64/chrome"))
            if not exe:
                raise
            b = pw.chromium.launch(executable_path=exe[-1])
        page = b.new_page(viewport={"width": 1100, "height": 760})
        errors = []
        page.on("pageerror", lambda e: errors.append(str(e)))
        page.goto(url)
        page.wait_for_selector("#gate:not(.hidden)")
        # wrong PIN, then the right one
        page.fill("#pin", "1111")
        page.click("#pinform button")
        page.wait_for_function("document.getElementById('gatemsg').textContent.length > 0")
        check("Wrong PIN" in page.inner_text("#gatemsg"), "wrong PIN message")
        page.fill("#pin", PIN)
        page.click("#pinform button")
        page.wait_for_selector("#app:not(.hidden)")
        page.wait_for_selector(".appcard .n")
        check(page.inner_text(".appcard .n") == "blinky", "app card shows blinky")

        # upload two files through the picker into /apps
        a2 = os.path.join(work, "drums.bin")
        fake_app(a2, "drums", "2.0.0")
        t2 = os.path.join(work, "kit.txt")
        open(t2, "w").write("kit")
        page.set_input_files("#picker", [a2, t2])
        page.wait_for_selector("td.name:has-text('drums.bin')")
        page.wait_for_selector("td.name:has-text('kit.txt')")
        page.wait_for_function("document.querySelectorAll('.appcard').length === 2")
        check(os.path.exists(os.path.join(dev.root, "apps", "drums.bin")), "upload landed on the device")

        # new folder (prompt), open it, go back via breadcrumbs
        page.once("dialog", lambda d: d.accept("Patches"))
        page.click("#b-mkdir")
        page.wait_for_selector("td.name.dir:has-text('Patches')")
        page.click("td.name.dir:has-text('Patches')")
        page.wait_for_selector("td.muted:has-text('empty')")
        page.click("#crumbs a:has-text('apps')")
        page.wait_for_selector("td.name:has-text('kit.txt')")

        # rename (prompt) and delete (confirm)
        row = page.locator("tr", has=page.locator("td.name", has_text="kit.txt"))
        page.once("dialog", lambda d: d.accept("kit-renamed.txt"))
        row.locator("button:has-text('Rename')").click()
        page.wait_for_selector("td.name:has-text('kit-renamed.txt')")
        row = page.locator("tr", has=page.locator("td.name", has_text="kit-renamed.txt"))
        page.once("dialog", lambda d: d.accept())
        row.locator("button:has-text('Delete')").click()
        page.wait_for_function("!document.body.innerText.includes('kit-renamed.txt')")
        check(not os.path.exists(os.path.join(dev.root, "apps", "kit-renamed.txt")), "deleted on device")

        page.screenshot(path=os.path.join(shot_dir, "web_app.png"), full_page=True)

        # launch from an app card (confirm)
        page.once("dialog", lambda d: d.accept())
        page.locator(".appcard", has_text="drums").locator("button").click()
        page.wait_for_selector("#toast:has-text('Launching')")
        check(dev.launched[-1] == "/apps/drums.bin", "launch from the web app")
        check(not errors, "no JS errors: %r" % errors)
        b.close()
    print("browser: PIN gate, upload x2, new folder, open/back, rename, delete, launch - "
          "screenshot tests/build/web_app.png")


def main():
    work = tempfile.mkdtemp(prefix="tulip-test-")
    root = os.path.join(work, "files")
    os.makedirs(root)
    port = free_port()
    srv, dev = web_mock.serve(root, port, PIN)
    try:
        test_cli(port, dev, work)
        test_browser(port, dev, work)
    finally:
        srv.shutdown()
        shutil.rmtree(work, ignore_errors=True)
    print("\n%d FAILURES" % failures if failures else "\nall web/cli tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
