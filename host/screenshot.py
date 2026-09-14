#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5", "pillow>=10"]
# ///
"""Capture the device framebuffer as PNG. Stop the launchd daemon first (it owns the port).

usage: screenshot.py out.png [--state working|done|needs_input|idle] [--out minor|major|none]
                     [--cmd tap,tap,...] [--live] [--http claude-status.local]
--live pushes the real payload from the daemon's build_payload(); otherwise a demo payload.
"""
import glob, importlib.util, json, os, struct, sys, time
import serial
from PIL import Image

ROOT = os.path.dirname(os.path.abspath(__file__))


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


def demo_payload():
    return {"out": "minor", "inc": "Elevated errors on Claude Cowork for Windows", "n": 2,
            "night": False, "hm": time.strftime("%I:%M%p").lstrip("0").lower(), "ctx": 27, "h5": 19, "h5r": "9:10pm", "h5m": 258,
            "wk": 53, "wkr": "Mon 9:00am", "wkm": 2408, "lim": False, "model": "Fable 5.1",
            "dir": "esp32_project", "eff": "high", "cost": 9.87, "dur": 58, "api": 21, "la": 412, "lr": 96, "tin": 1090, "tout": 31, "ch": 94, "cw": True, "ver": "2.1.270",
            "quiet": 70, "pace": 14,
            # 13 of the daemon's 32 weekly buckets, so the week is ~40% gone, ending at the
            # same 53% the WEEK gauge shows. That puts the last point 14 points above the
            # even-spend diagonal, which is what "pace": 14 claims.
            "hist": [0,4,8,13,17,22,27,31,36,40,45,49,53], "st": "working", "ts": int(time.time()), "age": 3}


def live_payload():
    spec = importlib.util.spec_from_file_location("d", os.path.join(ROOT, "claude_status_daemon.py"))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
    p = m.StatusPoller(); p.poll()
    return m.build_payload(p)


def settle(s, max_wait=8):
    """Opening the port can reset the board; wait for its hello line or 1.5 s of silence."""
    t0, last, buf = time.time(), time.time(), b""
    while time.time() - t0 < max_wait:
        chunk = s.read(512)
        if chunk:
            buf += chunk; last = time.time()
            if b'"hello"' in buf:
                time.sleep(0.3); s.read(4096); return
        elif time.time() - last > 1.5:
            return


def read_line(s, timeout=5):
    t0, buf = time.time(), b""
    while time.time() - t0 < timeout:
        c = s.read(1)
        if c == b"\n":
            return buf.decode(errors="ignore")
        buf += c
    return buf.decode(errors="ignore")


def decode(data, w, ht, out):
    img = Image.new("RGB", (w, ht))
    px = img.load()
    for i in range(w * ht):
        v = struct.unpack_from("<H", data, i * 2)[0]
        px[i % w, i // w] = ((v >> 11) << 3, ((v >> 5) & 0x3F) << 2, (v & 0x1F) << 3)
    img.save(out)
    print("saved", out, f"{w}x{ht}")


def main_http(out, host):
    import urllib.request
    token = ""
    try:
        token = open(os.path.expanduser("~/.claude/esp32-status/token")).read().strip()
    except OSError:
        pass
    hdr = {"X-Token": token, "Content-Type": "application/json"}
    p = live_payload() if "--live" in sys.argv else demo_payload()
    if arg("--state"): p["st"] = arg("--state")
    if arg("--out"): p["out"] = arg("--out")
    urllib.request.urlopen(urllib.request.Request(f"http://{host}/status", data=json.dumps(p).encode(), headers=hdr, method="POST"), timeout=5).read()
    for c in (arg("--cmd", "") or "").split(","):
        if c:
            urllib.request.urlopen(urllib.request.Request(f"http://{host}/cmd?c={c}", headers=hdr), timeout=5).read(); time.sleep(0.4)
    r = urllib.request.urlopen(urllib.request.Request(f"http://{host}/shot", headers=hdr), timeout=20)
    data = r.read()
    decode(data, int(r.headers["X-Width"]), int(r.headers["X-Height"]), out)


def main():
    out = sys.argv[1]
    if arg("--http"):
        return main_http(out, arg("--http"))
    port = sorted(glob.glob("/dev/cu.usbmodem*"))[0]
    s = serial.Serial(); s.port, s.baudrate, s.timeout = port, 115200, 0.5
    s.dtr = s.rts = False; s.open()
    settle(s)

    p = live_payload() if "--live" in sys.argv else demo_payload()
    if arg("--state"): p["st"] = arg("--state")
    if arg("--out"): p["out"] = arg("--out"); p["inc"] = "" if p["out"] == "none" else p["inc"]
    if "--lim" in sys.argv: p["lim"] = True
    s.write((json.dumps(p, separators=(",", ":")) + "\n").encode())
    print("device:", read_line(s))
    for c in (arg("--cmd", "") or "").split(","):
        if c:
            s.write(json.dumps({"cmd": c}).encode() + b"\n"); time.sleep(0.4)
    time.sleep(0.3); s.read(4096)
    s.write(b'{"cmd":"shot"}\n')
    hdr = read_line(s)
    while hdr and not hdr.startswith('{"shot"'):
        hdr = read_line(s)
    h = json.loads(hdr); print("hdr:", hdr.strip())
    w, ht, n = h["shot"], h["h"], h["len"]
    data, t0 = b"", time.time()
    while len(data) < n and time.time() - t0 < 20:
        data += s.read(n - len(data))
    s.close()
    if len(data) < n:
        sys.exit(f"short read {len(data)}/{n}")
    decode(data, w, ht, out)


if __name__ == "__main__":
    main()
