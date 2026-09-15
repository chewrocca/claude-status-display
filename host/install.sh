#!/bin/bash
# Full setup for the Claude status display on a fresh macOS machine.
# Idempotent: safe to re-run. Restores everything that lives outside this repo.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATE="$HOME/.claude/esp32-status"
AGENT="com.claude-status.display"
PLIST="$HOME/Library/LaunchAgents/$AGENT.plist"

say() { printf '  %s\n' "$*"; }

echo "Installing the Claude status display from $REPO"

# 1. state directories -------------------------------------------------------
mkdir -p "$STATE/sessions" "$STATE/attention" "$HOME/.claude/hooks" "$HOME/Library/LaunchAgents"
say "state directories ready"

# 2. the hook ----------------------------------------------------------------
install -m 755 "$REPO/host/esp32-status-hook.sh" "$HOME/.claude/hooks/esp32-status-hook.sh"
say "hook installed"

# 3. statusline mirror -------------------------------------------------------
# The statusline already receives everything the display needs, so rather than a second
# source of truth we mirror its payload to a per-session file the daemon reads.
SL="$HOME/.claude/statusline.sh"
if [[ ! -f "$SL" ]]; then
  say "NOTE: no ~/.claude/statusline.sh found; see README for the two lines to add"
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

# 4. hook registration in settings.json --------------------------------------
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

# 5. launchd agent -----------------------------------------------------------
UV="$(command -v uv || echo /opt/homebrew/bin/uv)"
cat > "$PLIST" <<EOF
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
  <dict><key>PATH</key><string>/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin</string></dict>
</dict>
</plist>
EOF
# bootout returns before launchd has finished tearing the job down, and bootstrapping into
# that gap fails with EIO ("Bootstrap failed: 5: Input/output error"), which is what a re-run
# on a machine that already had the agent loaded would hit. Wait for it to actually go.
launchctl bootout "gui/$(id -u)/$AGENT" 2>/dev/null || true
for _ in 1 2 3 4 5 6 7 8 9 10; do
  launchctl print "gui/$(id -u)/$AGENT" >/dev/null 2>&1 || break
  sleep 0.5
done
for attempt in 1 2 3; do
  if launchctl bootstrap "gui/$(id -u)" "$PLIST" 2>/dev/null; then
    say "daemon agent loaded"; break
  fi
  [ "$attempt" = 3 ] && say "NOTE: could not load the agent; run: launchctl bootstrap gui/\$(id -u) $PLIST"
  sleep 1
done

# A second machine needs none of the flashing: the board is already built and on the network.
# What it does need is the shared token, which only ever lives on the Mac that provisioned it.
if [[ ! -s "$STATE/token" ]]; then
  cat <<EOF

Done. This machine has no board token yet, so it cannot talk to a board over Wi-Fi.

If a board is already set up on another Mac, copy the token across and restart Claude Code:
    scp OTHER-MAC:~/.claude/esp32-status/token "$STATE/token"
    chmod 600 "$STATE/token"
That is all. The daemon finds no serial port here, so it will post to claude-status.local
by itself. Set CLAUDE_STATUS_HOST to the board's IP if mDNS is slow on your network.

If this is the first machine and you still have to build the board, see the README for the
flash and provisioning steps.
EOF
  exit 0
fi

cat <<EOF

Done. Remaining steps:
  1. Flash the board:
       arduino-cli core install esp32:esp32
       arduino-cli lib install "Adafruit GFX Library" "Adafruit ST7735 and ST7789 Library" \\
                               "Adafruit NeoPixel" "ArduinoJson"
       launchctl bootout gui/\$(id -u)/$AGENT
       arduino-cli compile --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc,PartitionScheme=no_ota \\
                           --build-path firmware/build firmware/claude_status
       arduino-cli upload  --fqbn esp32:esp32:esp32c6:CDCOnBoot=cdc,PartitionScheme=no_ota \\
                           --port /dev/cu.usbmodem* --input-dir firmware/build firmware/claude_status
       launchctl bootstrap gui/\$(id -u) $PLIST
  2. Wi-Fi (2.4 GHz only), if you want it off the laptop:
       uv run --script host/provision.py --list
       uv run --script host/provision.py --op "op://Vault/Item/Section/Field" --ssid "NAME"
  3. Restart Claude Code so the new hooks load.
EOF
