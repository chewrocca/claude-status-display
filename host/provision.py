#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5"]
# ///
"""Provision Wi-Fi on the board over USB. Prompts for SSID and password (hidden),
generates a shared token, sends everything straight to the board, and stores only
the token on this Mac (~/.claude/esp32-status/token, mode 600). Nothing is printed
or logged. Stop the launchd daemon first (it owns the serial port).

usage: provision.py --op REF --ssid X  password from 1Password (never printed)
       provision.py --list           show Wi-Fi networks this Mac has saved
       provision.py --auto --ssid X  password from the macOS keychain (no typing)
       provision.py --auto           same, for the network currently joined
       provision.py                  prompt for SSID and password (needs a real terminal)
       provision.py --forget         clear credentials on the board
       provision.py --status         show network status
--op takes a 1Password secret reference and shells out to the `op` CLI, e.g.
  --op "op://Private/Router/Wi-Fi/home-2.4" --ssid "your-2.4GHz-network"
--auto reads the System keychain, so macOS asks for your login password once; click
Allow. Either way the Wi-Fi password goes straight to the board: it is never printed,
logged, or written to disk on this Mac.

NOTE: the ESP32-C6 radio is 2.4 GHz ONLY. A 5 GHz-only SSID will never associate and
the board just sits at "connecting". Many routers use a separate name for 2.4 GHz.
"""
import getpass, glob, json, os, secrets, subprocess, sys, time
import serial

AGENT = "com.claude-status.display"


MACOS = sys.platform == "darwin"
UNIT = "claude-status-display"


def agent(action):
    """Pause / resume the daemon so this script can use the serial port.

    Whichever supervisor is holding it. The pkill is the backstop either way: a daemon started
    by hand answers to neither launchctl nor systemctl, and it is still sitting on the port.
    """
    if action == "stop":
        if MACOS:
            subprocess.run(["launchctl", "bootout", f"gui/{os.getuid()}/{AGENT}"], capture_output=True)
        else:
            subprocess.run(["systemctl", "--user", "stop", UNIT], capture_output=True)
        subprocess.run(["pkill", "-f", "claude_status_daemon"], capture_output=True)
        time.sleep(1)
    elif MACOS:
        plist = os.path.expanduser(f"~/Library/LaunchAgents/{AGENT}.plist")
        if os.path.exists(plist):
            subprocess.run(["launchctl", "bootstrap", f"gui/{os.getuid()}", plist], capture_output=True)
    else:
        subprocess.run(["systemctl", "--user", "start", UNIT], capture_output=True)

STATE = os.path.expanduser("~/.claude/esp32-status")
TOKEN_FILE = os.path.join(STATE, "token")


def open_port():
    # Same device under two naming schemes: macOS calls the CDC port after its driver, Linux
    # after its class.
    patterns = ["/dev/cu.usbmodem*"] if MACOS else ["/dev/ttyACM*", "/dev/ttyUSB*"]
    ports = sorted(p for pat in patterns for p in glob.glob(pat))
    if not ports:
        sys.exit("no board on USB")
    s = serial.Serial(); s.port, s.baudrate, s.timeout = ports[0], 115200, 0.5
    s.dtr = s.rts = False; s.open(); time.sleep(0.3); s.read(8192)
    return s


def wait_boot(s, secs=25):
    """Opening the port resets the board; wait for its hello before asking anything."""
    t0, buf = time.time(), b""
    while time.time() - t0 < secs:
        buf += s.read(1024)
        if b'"hello"' in buf:
            time.sleep(0.4); s.read(8192); return
    s.read(8192)


def ask(s, msg, wait=2.0):
    s.read(8192)
    s.write((json.dumps(msg) + "\n").encode()); time.sleep(wait)
    out = s.read(8192).decode(errors="ignore")
    return [l for l in out.splitlines() if l.startswith("{")]


def current_ssid():
    for cmd in (["ipconfig", "getsummary", "en0"], ["networksetup", "-getairportnetwork", "en0"]):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=10).stdout
        except Exception:
            continue
        for line in out.splitlines():
            if " SSID : " in line:
                v = line.split(" SSID : ", 1)[1].strip()
                return "" if v.startswith("<redacted>") else v   # macOS hides it without location permission
            if "Current Wi-Fi Network:" in line:
                return line.split(":", 1)[1].strip()
    return ""


SYSTEM_KEYCHAIN = "/Library/Keychains/System.keychain"


def saved_networks():
    try:
        out = subprocess.run(["networksetup", "-listpreferredwirelessnetworks", "en0"],
                             capture_output=True, text=True, timeout=10).stdout
    except Exception:
        return []
    return [l.strip() for l in out.splitlines()[1:] if l.strip()]


def keychain_password(ssid):
    # Wi-Fi passwords live in the System keychain; naming it explicitly is required.
    r = subprocess.run(["security", "find-generic-password", "-D", "AirPort network password",
                        "-a", ssid, "-w", SYSTEM_KEYCHAIN], capture_output=True, text=True)
    if r.returncode != 0:
        known = saved_networks()
        hint = f"\nSaved networks on this Mac:\n  " + "\n  ".join(known) if known else ""
        sys.exit(f"no saved password for {ssid!r}. Either the name differs, this Mac has never "
                 f"joined it, or you declined the keychain prompt.{hint}\n"
                 f"To type the password instead, run this in a Terminal window:\n"
                 f"  uv run --script host/provision.py")
    return r.stdout.rstrip("\n")


def op_password(ref):
    r = subprocess.run(["op", "read", ref], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"1Password read failed: {r.stderr.strip()[:200]}")
    pw = r.stdout.rstrip("\n")
    if not pw:
        sys.exit(f"1Password returned an empty value for {ref!r}")
    return pw


def credentials():
    if "--op" in sys.argv:
        ref = sys.argv[sys.argv.index("--op") + 1]
        if "--ssid" not in sys.argv:
            sys.exit("--op also needs --ssid NAME")
        ssid = sys.argv[sys.argv.index("--ssid") + 1]
        print(f"network: {ssid}  (password from 1Password)")
        return ssid, op_password(ref)
    if "--auto" in sys.argv:
        if not MACOS:
            sys.exit("--auto reads the macOS keychain and there is none here. Use --op with a "
                     "1Password reference, or run this without flags to type the password.")
        ssid = sys.argv[sys.argv.index("--ssid") + 1] if "--ssid" in sys.argv else current_ssid()
        if not ssid:
            sys.exit("could not read the current Wi-Fi network; pass --ssid NAME")
        print(f"network: {ssid}  (password from keychain, approve the dialog)")
        return ssid, keychain_password(ssid)
    if not sys.stdin.isatty():
        sys.exit("no terminal for prompts: run with --auto, or run this in a Terminal window")
    return input("Wi-Fi network name (SSID): ").strip(), getpass.getpass("Wi-Fi password (hidden): ")


def main():
    if "--list" in sys.argv:
        if not MACOS:
            sys.exit("--list reads the macOS Wi-Fi preferences and there is none here. Pass "
                     "--ssid NAME with --op, or run this without flags to type both.")
        nets = saved_networks()
        print("Wi-Fi networks saved on this Mac:")
        for n in nets:
            print("  " + n)
        print(f"\n{len(nets)} saved. Provision with:\n  uv run --script host/provision.py --auto --ssid \"NAME\"")
        return
    agent("stop")
    try:
        run()
    finally:
        agent("start")


def run():
    s = open_port()
    wait_boot(s)
    if "--forget" in sys.argv:
        print(ask(s, {"cmd": "wifi", "forget": True}))
        return
    if "--status" not in sys.argv:
        ssid, pw = credentials()
        token = secrets.token_urlsafe(24)
        os.makedirs(STATE, exist_ok=True)
        with open(TOKEN_FILE, "w") as f:
            f.write(token)
        os.chmod(TOKEN_FILE, 0o600)
        reply = ask(s, {"cmd": "wifi", "ssid": ssid, "pass": pw, "token": token})
        print("board:", [r for r in reply if "wifi" in r or "err" in r])
        print("waiting for the board to join the network (2.4 GHz only)...")
        for _ in range(20):
            time.sleep(1)
            lines = ask(s, {"cmd": "net"}, 0.8)
            st = next((json.loads(l) for l in lines if l.startswith('{"net"')), None)
            if st and st.get("net") == "up":
                break
        else:
            print("not joined. If that SSID is 5 GHz-only the board cannot see it:\n"
                  "the ESP32-C6 radio is 2.4 GHz only.")
    shown = False
    for l in ask(s, {"cmd": "net"}):
        if l.startswith('{"net"'):
            d = json.loads(l)
            print(f"network: {d['net']}  ssid: {d['ssid']}  ip: {d['ip']}  rssi: {d['rssi']} dBm")
            print(f"mdns: {d['mdns']}  ntp: {d['ntp']}  ble: {d['ble']}  clock: {d['clock']}  api: {d['own_out']}")
            shown = True
    for l in ask(s, {"cmd": "sd"}, 16.0):   # the speed ladder retries for a while when no card answers
        if l.startswith('{"sd"'):
            d = json.loads(l)
            if d.get("sd"):
                print(f"sd card: {d['type']}, {d['mb']} MB, linked at {d['hz'] // 1000} kHz")
            else:
                print("sd card: present but not responding (reseat it), or slot empty")
            shown = True
    if not shown:
        print("no reply from the board; try again")
    s.close()


if __name__ == "__main__":
    main()
