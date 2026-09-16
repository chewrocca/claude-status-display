#!/bin/bash
# Full setup for the Claude status display, on macOS or Linux.
# Idempotent: safe to re-run. Restores everything that lives outside this repo.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATE="$HOME/.claude/esp32-status"
AGENT="com.claude-status.display"
UNIT="claude-status-display"

say() { printf '  %s\n' "$*"; }

# 0. which machine is this ----------------------------------------------------
# Everything below this line is the same on every platform except starting the daemon, which
# is the one thing an operating system insists on owning. Decide once, here, and let the three
# svc_* functions be the only place that knows.
case "$(uname -s)" in
  Darwin) PLATFORM=macos ;;
  Linux)  PLATFORM=linux; command -v systemctl >/dev/null 2>&1 || PLATFORM=linux-nosystemd ;;
  *)      PLATFORM=unknown ;;
esac

echo "Installing the Claude status display from $REPO ($PLATFORM)"

case "$PLATFORM" in
  macos) SVC_PATH="$HOME/Library/LaunchAgents/$AGENT.plist" ;;
  linux) SVC_PATH="$HOME/.config/systemd/user/$UNIT.service" ;;
  *)     SVC_PATH="" ;;
esac

# How to ask for a missing tool. Saying "brew install" on a Debian box is worse than saying
# nothing, because it reads like the command to run.
pkg_hint() {
  case "$PLATFORM" in
    macos) echo "brew install$1" ;;
    *)
      if   command -v apt-get >/dev/null 2>&1; then echo "sudo apt-get install$1"
      elif command -v dnf     >/dev/null 2>&1; then echo "sudo dnf install$1"
      elif command -v pacman  >/dev/null 2>&1; then echo "sudo pacman -S$1"
      else echo "install:$1"
      fi ;;
  esac
}

# The PATH the service runs with. A user daemon starts with almost none, and uv is routinely
# somewhere only an interactive shell knows about.
svc_path_env() {
  case "$PLATFORM" in
    macos) echo "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin" ;;
    *)     echo "$HOME/.local/bin:$HOME/.cargo/bin:/usr/local/bin:/usr/bin:/bin" ;;
  esac
}

# 1. what this actually needs -------------------------------------------------
# Everything else here is written by this script, but the hook parses its input with jq and
# the daemon is a uv script. Say so plainly rather than installing something that cannot run.
MISSING=""
for t in jq uv python3; do command -v "$t" >/dev/null 2>&1 || MISSING="$MISSING $t"; done
if [[ -n "$MISSING" ]]; then
  say "MISSING:$MISSING"
  say "  jq is used by the hook, uv runs the daemon, python3 patches your settings."
  say "  $(pkg_hint "${MISSING/ python3/}")"
  say "  Continuing: everything will be installed, but will not run until those exist."
fi

# 2. state directories --------------------------------------------------------
mkdir -p "$STATE/sessions" "$STATE/attention" "$HOME/.claude/hooks"
[[ -n "$SVC_PATH" ]] && mkdir -p "$(dirname "$SVC_PATH")"
say "state directories ready"

# 3. the hook -----------------------------------------------------------------
install -m 755 "$REPO/host/esp32-status-hook.sh" "$HOME/.claude/hooks/esp32-status-hook.sh"
say "hook installed"

# 4. statusline mirror --------------------------------------------------------
# The statusline already receives everything the display needs, so rather than a second
# source of truth we mirror its payload to a per-session file the daemon reads.
SL="$HOME/.claude/statusline.sh"
if [[ ! -f "$SL" ]]; then
  say "NOTE: no ~/.claude/statusline.sh found; AGENTS.md has the two lines to add"
elif grep -q "esp32-status" "$SL"; then
  say "statusline already mirrors payloads"
else
  cp "$SL" "$SL.bak-esp32-$(date +%Y%m%d%H%M%S)"
  python3 - "$SL" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
if "session_id" not in s:
    print("  WARNING: statusline does not read .session_id; mirroring under 'default'")
mirror = '''# Mirror the payload for the ESP32 status display daemon (best-effort, cheap).
ESP_SID=$(printf '%s' "$input" | jq -r '.session_id // "default"' 2>/dev/null || echo default)
ESP_DIR="$HOME/.claude/esp32-status/sessions"
if [[ -d "$ESP_DIR" ]]; then
  printf '%s' "$input" > "$ESP_DIR/$ESP_SID.json.tmp" 2>/dev/null && mv -f "$ESP_DIR/$ESP_SID.json.tmp" "$ESP_DIR/$ESP_SID.json"
fi

'''
lines = s.split("\n")
for i, l in enumerate(lines):
    if l.startswith("input=") or "$(cat)" in l:
        lines.insert(i + 1, "\n" + mirror.rstrip("\n"))
        break
else:
    lines.insert(1, mirror)
open(p, "w").write("\n".join(lines))
PY
  say "statusline patched (backup kept alongside it)"
fi

# 5. hook registration in settings.json ---------------------------------------
python3 - "$HOME/.claude/settings.json" "$HOME/.claude/hooks/esp32-status-hook.sh" <<'PY'
import json, os, sys
path, cmd = sys.argv[1], sys.argv[2]
d = json.load(open(path)) if os.path.exists(path) else {}
hooks = d.setdefault("hooks", {})
for ev in list(hooks):                                  # drop any previous registration
    hooks[ev] = [e for e in hooks[ev] if not any(h.get("command") == cmd for h in e.get("hooks", []))]
    if not hooks[ev]:
        del hooks[ev]
def add(ev, matcher=None):
    e = {"hooks": [{"type": "command", "command": cmd}]}
    if matcher:
        e["matcher"] = matcher
    hooks.setdefault(ev, []).append(e)
add("Notification", "permission_prompt|idle_prompt|elicitation_dialog")
add("PermissionRequest"); add("PreToolUse", "AskUserQuestion")
for ev in ("Stop", "UserPromptSubmit", "SessionStart", "SessionEnd",
           "PostToolUse", "PostToolUseFailure", "PermissionDenied"):
    add(ev)
json.dump(d, open(path, "w"), indent=2); open(path, "a").write("\n")
print("  hooks registered:", ", ".join(sorted(hooks)))
PY

# 6. the daemon as a service --------------------------------------------------
UV="$(command -v uv || true)"
if [[ -z "$UV" ]]; then
  case "$PLATFORM" in
    macos) UV=/opt/homebrew/bin/uv ;;
    *)     UV="$HOME/.local/bin/uv" ;;
  esac
fi

svc_write() {
  case "$PLATFORM" in
    macos)
      cat > "$SVC_PATH" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$AGENT</string>
  <key>ProgramArguments</key>
  <array>
    <string>$UV</string><string>run</string><string>--script</string>
    <string>$REPO/host/claude_status_daemon.py</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>5</integer>
  <key>StandardOutPath</key><string>$STATE/daemon.log</string>
  <key>StandardErrorPath</key><string>$STATE/daemon.log</string>
  <key>EnvironmentVariables</key>
  <dict><key>PATH</key><string>$(svc_path_env)</string></dict>
</dict>
</plist>
EOF
      ;;
    linux)
      # A user unit, not a system one: this watches one person's Claude sessions and needs no
      # privilege. Restart=always is the launchd KeepAlive equivalent, and journald takes the
      # log, so there is no file to rotate.
      cat > "$SVC_PATH" <<EOF
[Unit]
Description=Claude Code status display
# Deliberately no After=network-online.target: that is a system target, not something the user
# manager has, and the daemon retries the board forever anyway. Ordering on it would look like
# it did something.

[Service]
ExecStart=$UV run --script $REPO/host/claude_status_daemon.py
Restart=always
RestartSec=5
Environment=PATH=$(svc_path_env)

[Install]
WantedBy=default.target
EOF
      ;;
  esac
}

svc_restart() {
  case "$PLATFORM" in
    macos)
      # bootout returns before launchd has finished tearing the job down, and bootstrapping
      # into that gap fails with EIO ("Bootstrap failed: 5: Input/output error"), which is
      # what a re-run on a machine that already had the agent loaded would hit. Wait for it.
      launchctl bootout "gui/$(id -u)/$AGENT" 2>/dev/null || true
      for _ in 1 2 3 4 5 6 7 8 9 10; do
        launchctl print "gui/$(id -u)/$AGENT" >/dev/null 2>&1 || break
        sleep 0.5
      done
      for attempt in 1 2 3; do
        if launchctl bootstrap "gui/$(id -u)" "$SVC_PATH" 2>/dev/null; then
          say "daemon agent loaded"; return 0
        fi
        [ "$attempt" = 3 ] && say "NOTE: could not load the agent; run: launchctl bootstrap gui/\$(id -u) $SVC_PATH"
        sleep 1
      done ;;
    linux)
      systemctl --user daemon-reload 2>/dev/null || true
      if systemctl --user enable --now "$UNIT.service" >/dev/null 2>&1; then
        systemctl --user restart "$UNIT.service" >/dev/null 2>&1 || true
        say "daemon service started (journalctl --user -u $UNIT -f)"
      else
        say "NOTE: could not start the service; run: systemctl --user enable --now $UNIT"
      fi
      # Without lingering the service dies at logout and never starts on boot, which reads as
      # the display simply forgetting this machine exists. Best-effort: it may want a password.
      if command -v loginctl >/dev/null 2>&1; then
        loginctl enable-linger "$(id -un)" >/dev/null 2>&1 \
          || say "NOTE: run 'sudo loginctl enable-linger $(id -un)' so it survives logout"
      fi ;;
    linux-nosystemd)
      say "no systemd here, so nothing was registered to start it. Run the daemon with:"
      say "  $UV run --script $REPO/host/claude_status_daemon.py" ;;
    unknown)
      say "unrecognised platform ($(uname -s)); the hook and state are installed, but nothing"
      say "was registered to start the daemon. Run it yourself with:"
      say "  $UV run --script $REPO/host/claude_status_daemon.py" ;;
  esac
}

# A one-word control for the daemon, written here so it exists on every machine however it was
# onboarded and whatever supervises it. Remembering "launchctl kickstart -k gui/$(id -u)/..." on
# one machine and "systemctl --user restart ..." on the next is the kind of thing that makes a
# desk gadget feel like infrastructure.
svc_write_helper() {
  cat > "$STATE/svc" <<EOF
#!/bin/bash
# Control the Claude status display daemon. Written by install.sh; edits will be overwritten.
set -euo pipefail
A="\${1:-restart}"
case "\$A" in
  start|stop|restart|status|log) ;;
  *) echo "usage: \$0 [start|stop|restart|status|log]" >&2; exit 2 ;;
esac
EOF
  case "$PLATFORM" in
    macos)
      cat >> "$STATE/svc" <<EOF
G="gui/\$(id -u)/$AGENT"
case "\$A" in
  start)   launchctl bootstrap "gui/\$(id -u)" "$SVC_PATH" ;;
  stop)    launchctl bootout "\$G" ;;
  restart) launchctl kickstart -k "\$G" ;;
  status)  launchctl print "\$G" | sed -n 's/^[[:space:]]*state = /state: /p;s/^[[:space:]]*pid = /pid: /p' ;;
  log)     tail -f "$STATE/daemon.log" ;;
esac
EOF
      ;;
    linux)
      cat >> "$STATE/svc" <<EOF
case "\$A" in
  start)   systemctl --user start $UNIT ;;
  stop)    systemctl --user stop $UNIT ;;
  restart) systemctl --user restart $UNIT ;;
  status)  systemctl --user --no-pager status $UNIT ;;
  log)     journalctl --user -u $UNIT -f ;;
esac
EOF
      ;;
    *)
      cat >> "$STATE/svc" <<EOF
echo "No service manager was registered on this machine. Run the daemon with:" >&2
echo "  $UV run --script $REPO/host/claude_status_daemon.py" >&2
exit 1
EOF
      ;;
  esac
  chmod 755 "$STATE/svc"
  say "control script: $STATE/svc [start|stop|restart|status|log]"
}

if [[ -n "$SVC_PATH" ]]; then
  svc_write
fi
svc_write_helper
svc_restart

# A board on USB is a serial device, and on Linux reading one is a group membership rather
# than something anybody is born with. Say so here rather than let the daemon look broken.
if [[ "$PLATFORM" == linux* ]]; then
  for dev in /dev/ttyACM* /dev/ttyUSB*; do
    [[ -e "$dev" ]] || continue
    if [[ ! -r "$dev" ]]; then
      grp="$(stat -c '%G' "$dev" 2>/dev/null || echo dialout)"
      say "NOTE: $dev is not readable by you. For the USB link: sudo usermod -aG $grp $(id -un)"
      say "  (log out and back in afterwards). Wi-Fi needs none of this."
    fi
    break
  done
fi

# A second machine needs none of the flashing: the board is already built and on the network.
# What it does need is the shared token, which only ever lives on the machine that provisioned it.
if [[ ! -s "$STATE/token" ]]; then
  cat <<EOF

Done. This machine has no board token yet, so it cannot talk to a board over Wi-Fi.

If a board is already set up elsewhere, copy the token across and restart Claude Code:
    scp OTHER-MACHINE:~/.claude/esp32-status/token "$STATE/token"
    chmod 600 "$STATE/token"
That is all. The daemon finds no serial port here, so it will post to claude-status.local
by itself. Set CLAUDE_STATUS_HOST to the board's IP if mDNS is slow on your network.

Easier: press BOOT on the board round to the Enroll page and run the one command it shows.

If this is the first machine and you still have to build the board, see the README for the
flash and provisioning steps.
EOF
  exit 0
fi

# Invoked by the board's own enrollment script: the board is built, on the network, and just
# handed over its token. Telling this machine how to flash one would be nonsense.
if [[ -n "${CLAUDE_STATUS_ENROLLED:-}" ]]; then
  cat <<EOF

Enrolled. Restart Claude Code so the hooks load.
EOF
  exit 0
fi

cat <<EOF

Done. Remaining steps:
  1. Flash the board (needs a Mac or Linux box with arduino-cli and a USB cable):
       arduino-cli core install esp32:esp32
       arduino-cli lib install "Adafruit GFX Library" "Adafruit ST7735 and ST7789 Library" \\
                               "Adafruit NeoPixel" "ArduinoJson"
       $(if [[ "$PLATFORM" == macos ]]; then echo "launchctl bootout gui/\$(id -u)/$AGENT"; else echo "systemctl --user stop $UNIT"; fi)
       arduino-cli compile --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc,PartitionScheme=no_ota \\
                           --build-path firmware/build firmware/claude_status
       arduino-cli upload  --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc,PartitionScheme=no_ota \\
                           --port $(if [[ "$PLATFORM" == macos ]]; then echo "/dev/cu.usbmodem*"; else echo "/dev/ttyACM0"; fi) --input-dir firmware/build firmware/claude_status
       $(if [[ "$PLATFORM" == macos ]]; then echo "launchctl bootstrap gui/\$(id -u) $SVC_PATH"; else echo "systemctl --user start $UNIT"; fi)
  2. Wi-Fi (2.4 GHz only), if you want it off the laptop:
       uv run --script host/provision.py --list
       uv run --script host/provision.py --op "op://Vault/Item/Section/Field" --ssid "NAME"
  3. Restart Claude Code so the new hooks load.
EOF
