#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5"]
# ///
"""Push Claude Code status to the ESP32-C6 display over USB serial.

Inputs (all under ~/.claude/esp32-status/):
  sessions/<session_id>.json   raw statusline payload, written by ~/.claude/statusline.sh
  attention/<session_id>.json  {"state": working|done|needs_input, "ts": epoch}
                               written by ~/.claude/hooks/esp32-status-hook.sh
Plus the public Claude status page for outage detection.

Payload (one JSON line, on change or every HEARTBEAT_S):
  ctx h5 wk      used %            h5r wkr   reset time (12h Central)
  h5m wkm        minutes to reset  st        working|done|needs_input|idle
  lim            any limit at 100  out       none|minor|major|critical|unknown
  inc            incident title    n         active sessions
  night          22:00-07:00       hm        host clock, e.g. 4:31pm
  ts             epoch of newest data (device uses it for staleness)
  age model dir cost
"""
import glob, json, os, sys, threading, time, urllib.request
from datetime import datetime
from zoneinfo import ZoneInfo

import serial

STATE = os.path.expanduser("~/.claude/esp32-status")
SESSIONS = os.path.join(STATE, "sessions")
ATTENTION = os.path.join(STATE, "attention")
STATUS_HOSTS = ["https://status.claude.com", "https://status.anthropic.com"]
# Set CLAUDE_STATUS_PORT_GLOB to a path that matches nothing to force the Wi-Fi path.
PORT_GLOB = os.environ.get("CLAUDE_STATUS_PORT_GLOB", "/dev/cu.usbmodem*")
BOARD_HOST = os.environ.get("CLAUDE_STATUS_HOST", "claude-status.local")   # Wi-Fi fallback when USB is absent
TOKEN_FILE = os.path.join(STATE, "token")
HTTP_RETRY_S = 10
HTTP_TIMEOUT_S = 8        # mDNS resolution alone can take 5 s on a cold cache
SERIAL_COOLDOWN_S = 300   # a port that never acknowledges is not our board; stop poking it
HISTORY_FILE = os.path.join(STATE, "history.json")
HISTORY_EVERY_S = 300     # one sample per 5 minutes is plenty for a 7-day window
HISTORY_POINTS = 32       # samples sent to the device; it has 208 px to draw them in
WEEK_S = 7 * 24 * 3600
QUIET_BELOW = 70          # a gauge under this needs no space; nothing is decided at 31%
MAX_SESSIONS = 4          # rows that fit the panel; the rest are summarised as a count
ABANDONED_AFTER_S = 1800  # finished this long ago is not waiting on you, it is over
SESSION_LIST_TTL_S = 7200 # a window silent this long is closed, not idle; drop it from the list
STATUS_POLL_S = 90
# Components whose health drives the display; everything else is reported as "other".
WATCH = {"Claude API (api.anthropic.com)": "API", "Claude Code": "CODE"}
LEVEL = {"operational": "none", "under_maintenance": "minor", "degraded_performance": "minor",
         "partial_outage": "major", "major_outage": "critical"}
RANK = {"none": 0, "minor": 1, "major": 2, "critical": 3, "unknown": 0}
HEARTBEAT_S = 3
SESSION_TTL_S = 24 * 3600
IDLE_AFTER_S = 30 * 60
NIGHT_START, NIGHT_END = 22, 7
# Display timezone; DST transitions come from the system zone database.
TZ = ZoneInfo(os.environ.get("CLAUDE_STATUS_TZ", "America/Chicago"))
VERBOSE = "--verbose" in sys.argv


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def read_json(path):
    try:
        with open(path) as f:
            return json.load(f)
    except Exception:
        return None


def fresh_files(folder):
    """(path, mtime) for JSON files newer than SESSION_TTL_S; prunes older ones."""
    now, out = time.time(), []
    for f in glob.glob(os.path.join(folder, "*.json")):
        try:
            m = os.path.getmtime(f)
        except OSError:
            continue
        if now - m > SESSION_TTL_S:
            try: os.remove(f)
            except OSError: pass
            continue
        out.append((f, m))
    return out


def latest_session():
    files = fresh_files(SESSIONS)
    if not files:
        return None, 0
    f, m = max(files, key=lambda t: t[1])
    return read_json(f), m


def attention():
    """Aggregate per-session attention: any needs_input > any done > any working > idle."""
    states, newest, now = [], 0, time.time()
    for f, m in fresh_files(ATTENTION):
        d = read_json(f) or {}
        try:
            ts = float(d.get("ts", m))
        except (TypeError, ValueError):
            ts = m
        if now - max(m, ts) > IDLE_AFTER_S:   # session died without SessionEnd
            continue
        states.append(d.get("state") or "idle")   # a truncated/garbled file must not shout
        newest = max(newest, ts)
    for s in ("needs_input", "done", "working"):
        if s in states:
            return s, newest, len(states)
    return "idle", newest, len(states)


def fmt_reset(epoch, with_day=False):
    """12-hour Central time, e.g. '9:10pm' or 'Mon 9:00am'."""
    try:
        dt = datetime.fromtimestamp(float(epoch), TZ)
    except Exception:
        return ""
    t = f"{dt.strftime('%I').lstrip('0')}:{dt.strftime('%M')}{dt.strftime('%p').lower()}"
    return f"{dt.strftime('%a')} {t}" if with_day else t


def pct(v):
    try:
        return int(float(v))
    except Exception:
        return -1


def limit(window, with_day):
    """(used %, reset label, minutes remaining). Zeroed once the reset time has passed."""
    used = pct(window.get("used_percentage"))
    try:
        reset = float(window.get("resets_at"))
    except (TypeError, ValueError):
        return used, "", -1
    mins = int((reset - time.time()) / 60)
    if mins <= 0:
        return used, "", -1
    return used, fmt_reset(reset, with_day), mins


class StatusPoller(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.indicator, self.incident, self.comp, self.other, self.failures = "unknown", "", "", "", 0
        self.wake = threading.Event()

    def force(self):
        self.wake.set()

    def run(self):
        while True:
            self.poll()
            self.wake.wait(STATUS_POLL_S)
            self.wake.clear()

    def fetch(self, url):
        req = urllib.request.Request(url, headers={"User-Agent": "claude-esp32-status"})
        with urllib.request.urlopen(req, timeout=8) as r:
            return json.load(r)

    def poll(self):
        for host in STATUS_HOSTS:
            try:
                d = self.fetch(f"{host}/api/v2/summary.json")
                worst, comp, others = "none", "", []
                for c in d.get("components") or []:
                    lvl = LEVEL.get(c.get("status"), "minor")
                    name = c.get("name", "")
                    if name in WATCH:
                        if RANK[lvl] > RANK[worst]:
                            worst, comp = lvl, WATCH[name]
                    elif lvl != "none":
                        others.append(f"{name.split(' (')[0]} {c.get('status', '').replace('_', ' ')}")
                self.other = "; ".join(others)[:80]
                self.incident = ""
                for inc in d.get("incidents") or []:
                    if (inc.get("status") or "") in ("resolved", "postmortem"):
                        continue
                    names = {x.get("name") for x in inc.get("components") or []}
                    if not (names & set(WATCH)):
                        continue
                    self.incident = (inc.get("name") or "")[:120]
                    # Statuspage can leave a component green while an incident against it is
                    # still open. Treat the open incident as at least a degradation.
                    lvl = {"critical": "critical", "major": "major"}.get(inc.get("impact") or "", "minor")
                    if RANK[lvl] > RANK[worst]:
                        worst, comp = lvl, WATCH[sorted(names & set(WATCH))[0]]
                    break
                self.indicator, self.comp, self.failures = worst, comp, 0
                if VERBOSE:
                    log(f"status {self.indicator} {self.comp} inc={self.incident!r} other={self.other!r}")
                return
            except Exception as e:
                log(f"status fetch failed ({host}): {e}")
        self.failures += 1
        if self.failures >= 3:
            self.indicator, self.incident, self.comp, self.other = "unknown", "", "", ""


def short_model(name):
    """Tidy the model name for the band: 'Opus 5 (1M context)' -> 'Opus 5'."""
    n = (name or "").split(" (")[0].strip()
    if n.lower().startswith("claude "):
        n = n[7:]                      # the device is obviously showing Claude
    return n[:12]


def session_stats(sl):
    """Session figures for the Stats page (mirrors the useful part of /usage)."""
    c, cw, pc = sl.get("cost") or {}, sl.get("context_window") or {}, sl.get("prompt_cache") or {}
    def num(v):
        try: return float(v or 0)
        except (TypeError, ValueError): return 0.0
    return {
        "dur": int(num(c.get("total_duration_ms")) / 60000),        # minutes of wall time
        "api": int(num(c.get("total_api_duration_ms")) / 60000),    # minutes waiting on the API
        "la": int(num(c.get("total_lines_added"))), "lr": int(num(c.get("total_lines_removed"))),
        "tin": int(num(cw.get("total_input_tokens")) / 1000), "tout": int(num(cw.get("total_output_tokens")) / 1000),
        "ch": int(num(pc.get("hit_ratio")) * 100) if pc.get("hit_ratio") is not None else -1,
        "cw": bool(pc.get("warm")),
        "ver": str(sl.get("version") or "")[:8],
    }


def window_label(size):
    """1000000 -> '1M', 200000 -> '200K'. A context percentage means different things
    depending on how big the window is, so the size belongs next to it."""
    try:
        n = int(size or 0)
    except (TypeError, ValueError):
        return ""
    if n <= 0:
        return ""
    if n >= 1_000_000:
        m = n / 1_000_000
        return f"{m:.0f}M" if abs(m - round(m)) < 0.05 else f"{m:.1f}M"
    if n >= 1000:
        return f"{n // 1000}K"
    return str(n)


def session_rows():
    """One row per live session, ranked by who is blocked and for how long.

    The ordering is the product: the top row is always the thing to do next. Wait time
    separates a session you are actively answering from one you forgot existed, and a
    finished session past ABANDONED_AFTER_S is a graveyard entry rather than a to-do.
    """
    now = time.time()
    att = {}
    for f, m in fresh_files(ATTENTION):
        d = read_json(f) or {}
        try:
            ts = float(d.get("ts", m))
        except (TypeError, ValueError):
            ts = m
        if now - max(m, ts) > SESSION_TTL_S:
            continue
        att[os.path.splitext(os.path.basename(f))[0]] = (d.get("state") or "idle", ts)

    rows = []
    for f, m in fresh_files(SESSIONS):
        sid = os.path.splitext(os.path.basename(f))[0]
        sl = read_json(f) or {}
        state, ts = att.get(sid, ("idle", m))
        wait = int(now - ts)
        # No hook has fired and nothing has been written for hours: that window is closed,
        # not idle. Listing it is clutter on a four-row page.
        if sid not in att and now - m > SESSION_LIST_TTL_S:
            continue
        if state == "done" and wait > ABANDONED_AFTER_S:
            state = "over"                      # finished long ago: not waiting on you
        name = (sl.get("session_name")
                or os.path.basename((sl.get("workspace") or {}).get("current_dir") or "")
                or sid[:8])
        rows.append({
            "n": name[:12],
            "s": {"needs_input": "n", "done": "d", "working": "w", "over": "o", "idle": "i"}.get(state, "i"),
            "w": wait,
            "c": round(float((sl.get("cost") or {}).get("total_cost_usd") or 0), 2),
            "x": pct((sl.get("context_window") or {}).get("used_percentage")),
        })

    # needs you first, then finished and waiting, then working, then over. Longest wait wins
    # inside each band, because that is the one you have forgotten about.
    order = {"n": 0, "d": 1, "w": 2, "o": 3, "i": 4}
    rows.sort(key=lambda r: (order.get(r["s"], 9), -r["w"]))
    return rows


def record_history(weekly_pct, weekly_reset):
    """Append a weekly-usage sample, pruned to the current window. Returns (series, pace).

    pace is percentage points ahead of a linear burn: +12 means you have spent 12 points
    more of the weekly budget than simply being this far through the week would predict.
    That is the number worth acting on. The raw percentage is not.
    """
    now = time.time()
    try:
        hist = json.load(open(HISTORY_FILE))
        if not isinstance(hist, list):
            hist = []
    except Exception:
        hist = []

    if not weekly_reset:
        return [], None
    start = weekly_reset - WEEK_S
    # A reset drops everything from the previous window; the series is per window.
    hist = [h for h in hist if isinstance(h, list) and len(h) == 2 and start <= h[0] <= now]
    if not hist or now - hist[-1][0] >= HISTORY_EVERY_S:
        hist.append([int(now), int(weekly_pct)])
        try:
            tmp = HISTORY_FILE + ".tmp"
            with open(tmp, "w") as f:
                json.dump(hist[-4000:], f)
            os.replace(tmp, HISTORY_FILE)
        except OSError as e:
            log(f"history write failed: {e}")

    elapsed = max(0.0, min(1.0, (now - start) / WEEK_S))
    pace = int(round(weekly_pct - elapsed * 100))

    # Downsample to a fixed width by bucketing over the window, so the x axis is time
    # rather than sample count and a gap in sampling reads as a flat stretch.
    if len(hist) < 2:
        return [], pace
    series = []
    last = hist[0][1]
    for i in range(HISTORY_POINTS):
        lo = start + WEEK_S * i / HISTORY_POINTS
        hi = start + WEEK_S * (i + 1) / HISTORY_POINTS
        if lo > now:
            break
        vals = [v for t, v in hist if lo <= t < hi]
        last = max(vals) if vals else last
        series.append(last)
    return series, pace


def build_payload(poller):
    sl, mtime = latest_session()
    state, att_ts, n = attention()
    now = time.time()
    local = datetime.now(TZ)
    p = {
        "out": poller.indicator, "inc": poller.incident, "comp": poller.comp, "other": poller.other, "n": n,
        "night": local.hour >= NIGHT_START or local.hour < NIGHT_END,
        "hm": fmt_reset(now),
    }
    ts = max(mtime, att_ts)
    if sl:
        rl = sl.get("rate_limits") or {}
        h5, h5r, h5m = limit(rl.get("five_hour") or {}, False)
        wk, wkr, wkm = limit(rl.get("seven_day") or {}, True)
        p.update({
            "ctx": pct((sl.get("context_window") or {}).get("used_percentage")),
            "cwin": window_label((sl.get("context_window") or {}).get("context_window_size")),
            "h5": h5, "h5r": h5r, "h5m": h5m,
            "wk": wk, "wkr": wkr, "wkm": wkm,
            "lim": h5 >= 100 or wk >= 100,
            "model": short_model((sl.get("model") or {}).get("display_name")),
            "eff": ((sl.get("effort") or {}).get("level") or "")[:10],
            "dir": (sl.get("session_name") or os.path.basename((sl.get("workspace") or {}).get("current_dir") or ""))[:20],
            "cost": round(float((sl.get("cost") or {}).get("total_cost_usd") or 0), 2),
            **session_stats(sl),
        })
        try:
            reset_at = float((rl.get("seven_day") or {}).get("resets_at") or 0)
        except (TypeError, ValueError):
            reset_at = 0
        if wk >= 0 and reset_at:
            series, pace = record_history(wk, reset_at)
            if series:
                p["hist"] = series
            if pace is not None:
                p["pace"] = pace
        p["quiet"] = QUIET_BELOW
    rows = session_rows()
    if rows:
        p["sess"] = rows[:MAX_SESSIONS]
        p["nsess"] = len(rows)
        blocked = [r for r in rows if r["s"] == "n"]
        p["nblk"] = len(blocked)               # the LED pulses this many times
        p["blkw"] = max((r["w"] for r in blocked), default=0)
        # Another window still running means the desk is not idle, whatever the headline
        # session is doing. Without this a finished session screensavers over a live one.
        p["nwork"] = sum(1 for r in rows if r["s"] == "w")
    if not ts or now - ts > IDLE_AFTER_S:
        state = "idle"
    p["st"] = state
    p["ts"] = int(ts)
    p["age"] = int(now - ts) if ts else -1
    return p


def read_token():
    try:
        with open(TOKEN_FILE) as f:
            return f.read().strip()
    except OSError:
        return ""


class HttpLink:
    """Push payloads to the board over the LAN (http://claude-status.local/status)."""
    def __init__(self):
        self.ok, self.next_try, self.token = False, 0, read_token()
        self.host = BOARD_HOST          # swapped for the numeric IP once we learn it, mDNS is slow

    def send(self, payload):
        if time.time() < self.next_try:
            return False
        body = json.dumps(payload, separators=(",", ":")).encode()
        req = urllib.request.Request(f"http://{self.host}/status", data=body, method="POST",
                                     headers={"Content-Type": "application/json", "X-Token": self.token})
        try:
            with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT_S) as r:
                r.read()
            if not self.ok:
                log(f"wifi link up ({self.host})")
                self.learn_ip()
            self.ok = True
            return True
        except Exception as e:
            if self.ok:
                log(f"wifi link lost: {e}")
            self.ok = False
            self.host = BOARD_HOST       # fall back to the name; the IP may have changed
            self.next_try = time.time() + HTTP_RETRY_S
            return False

    def learn_ip(self):
        """Ask the board its own IP and use that from now on, so mDNS is not in the hot path."""
        try:
            req = urllib.request.Request(f"http://{self.host}/info", headers={"X-Token": self.token})
            with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT_S) as r:
                ip = json.load(r).get("ip")
            if ip and ip != "0.0.0.0":
                self.host = ip
                log(f"wifi link pinned to {ip}")
        except Exception:
            pass


def open_port():
    ports = sorted(glob.glob(PORT_GLOB))
    if not ports:
        return None
    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.write_timeout = ports[0], 115200, 0, 2
    s.dtr = False   # keep DTR/RTS low so opening the port does not reset the ESP32
    s.rts = False
    s.open()
    log(f"connected {ports[0]}")
    return s


def handle_device(ser, poller, buf):
    """Read {"cmd": ...} lines from the device."""
    try:
        data = ser.read(512)
    except Exception:
        return buf
    if not data:
        return buf
    buf += data.decode(errors="ignore")
    while "\n" in buf:
        line, buf = buf.split("\n", 1)
        try:
            msg = json.loads(line)
        except Exception:
            continue
        if msg.get("cmd") == "poll":
            poller.force()
        elif "ok" in msg and not handle_device.acked:
            handle_device.acked = True
            log(f"device acknowledged payload ts={msg.get('ok')} st={msg.get('st')}")
        elif "hello" in msg:
            log(f"device hello fw={msg.get('fw')}")
        elif "err" in msg:
            log(f"device rejected payload: {msg}")
        if VERBOSE:
            log(f"device: {msg}")
    return buf[-1024:]


handle_device.acked = False


def main():
    os.makedirs(SESSIONS, exist_ok=True)
    os.makedirs(ATTENTION, exist_ok=True)
    poller = StatusPoller()
    poller.start()
    http = HttpLink()
    ser, last_sent, last_key, buf = None, 0, None, ""
    serial_since = 0.0
    serial_blocked_until = 0.0
    while True:
        try:
            if ser is None and time.time() >= serial_blocked_until:
                ser = open_port()
                if ser is not None:
                    serial_since = time.time()
                    handle_device.acked = False
            if ser is not None:
                buf = handle_device(ser, poller, buf)
                # If nothing on that port ever acknowledges, it is not our board. Drop it
                # and let the Wi-Fi path take over instead of writing into the void.
                if not handle_device.acked and time.time() - serial_since > 20:
                    # A wedged USB stack still enumerates, so the port opens and swallows
                    # writes. Back off rather than reopening it every twenty seconds.
                    log(f"serial port never acknowledged; using wifi, retrying in {SERIAL_COOLDOWN_S // 60} min")
                    try: ser.close()
                    except Exception: pass
                    ser, last_key = None, None
                    serial_blocked_until = time.time() + SERIAL_COOLDOWN_S
            payload = build_payload(poller)
            key = {k: v for k, v in payload.items() if k not in ("age", "hm")}
            now = time.time()
            if key != last_key or now - last_sent >= HEARTBEAT_S:
                if ser is not None:
                    ser.write((json.dumps(payload, separators=(",", ":")) + "\n").encode())
                    last_sent, last_key = now, key
                elif http.send(payload):
                    last_sent, last_key = now, key
                if VERBOSE:
                    log(payload)
            time.sleep(0.5 if ser is not None else 1.0)
        except (serial.SerialException, OSError) as e:
            log(f"serial error: {e}")
            try: ser and ser.close()
            except Exception: pass
            ser, last_key, handle_device.acked = None, None, False
            time.sleep(2)
        except Exception as e:
            log(f"unexpected: {e!r}")
            time.sleep(1)


if __name__ == "__main__":
    main()
