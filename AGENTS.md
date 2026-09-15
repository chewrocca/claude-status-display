# AGENTS.md

You are probably here because someone asked you to build something like this. Good. This
file is written to be executed, not admired.

**Do not port this firmware.** The board is incidental. What is worth taking is that Claude
Code already publishes everything worth showing, through two documented interfaces, and
almost nobody surfaces it anywhere except a terminal they are not looking at. Read
[The pattern](#the-pattern), then build the version that fits whatever display your human
already owns.

## The pattern

A long-running agent knows two things its user wants at a glance and cannot see:

1. **Is it working, or is it waiting on me?** This is the one that keeps pulling attention
   back to a window.
2. **How much budget is left?** Context window, and the subscription rate-limit windows.

Both are already on disk, or one hook away. The entire glue is about thirty lines. Everything
else in this repository is one answer to what to do with it.

## Where the data actually comes from

No API key. No polling of Anthropic. No scraping.

### Status line: the numbers

Claude Code runs a command on every update and hands it a JSON object on stdin. Configure it
in `~/.claude/settings.json`:

```json
{ "statusLine": { "type": "command", "command": "~/.claude/statusline.sh" } }
```

The fields that matter:

```jsonc
{
  "session_id": "uuid",
  "model":   { "display_name": "Opus 5" },
  "effort":  { "level": "low|medium|high|xhigh|max" },
  "workspace": { "current_dir": "/path/to/project" },
  "session_name": "optional, set with /rename",
  "context_window": { "used_percentage": 57, "remaining_percentage": 43 },
  "cost": { "total_cost_usd": 92.0, "total_duration_ms": 0, "total_api_duration_ms": 0,
            "total_lines_added": 0, "total_lines_removed": 0 },
  "prompt_cache": { "hit_ratio": 0.93, "warm": true },
  "rate_limits": {                       // Pro/Max subscriptions ONLY
    "five_hour": { "used_percentage": 22, "resets_at": 1789265400 },
    "seven_day": { "used_percentage": 64, "resets_at": 1789394400 }
  }
}
```

If your script already renders a prompt, do not write a second one. Add two lines that also
mirror the raw payload to a file keyed by session:

```bash
ESP_SID=$(printf '%s' "$input" | jq -r '.session_id // "default"')
printf '%s' "$input" > "$STATE/sessions/$ESP_SID.json.tmp" && mv -f "$STATE/sessions/$ESP_SID.json.tmp" "$STATE/sessions/$ESP_SID.json"
```

**`rate_limits` is absent on API-key billing.** Degrade gracefully; do not show empty bars as
if they were zero.

### Hooks: the attention state

The status line cannot tell you Claude is *blocked*. Hooks can. Register one script against
several events and switch on `hook_event_name`:

| Event | Matcher | Means |
|---|---|---|
| `Stop` | — | Turn finished. Your turn. |
| `Notification` | `permission_prompt`, `elicitation_dialog` | A dialog has been waiting ~6 s. |
| `PermissionRequest` | — | Fires *before* the prompt renders. |
| `PreToolUse` | `AskUserQuestion` | Claude is asking a question directly. |
| `UserPromptSubmit`, `PostToolUse` | — | Working. |
| `SessionEnd` | — | Gone; delete the session's file. |

Two things that will bite you:

- **Subagents fire `PostToolUse` too.** They carry `agent_id`; ignore those events or a busy
  subagent masks the main session's state.
- **There is no "permission approved" event.** After the user approves, the state stays
  `needs_input` until the tool finishes and `PostToolUse` fires. Either accept it or add a
  timeout.

Write per session, atomically, and build the JSON with a real encoder because tool names can
contain quotes:

```bash
tmp=$(mktemp "$dir/$sid.XXXXXX")
jq -n --arg state "$state" --argjson ts "$(date +%s)" '{state:$state, ts:$ts}' > "$tmp" && mv -f "$tmp" "$dir/$sid.json"
```

### Merging

With several sessions open, the rule that works:

- **Attention is a union, but only blocked outranks working.** If *any* live session needs
  input, say so: showing only the focused session hides the one that is blocked. A session
  that merely *finished* is different. Ranking it above live work means a window you walked
  away from holds the display at "your turn" while another is visibly working, which reads
  as the device lying to you. Order is `needs_input > working > done`, and the count of
  finished sessions rides along so nothing is hidden.
- **Anything that can set the headline must appear in the list.** A window can fire hooks
  without ever mirroring its status line. Build the session list from the union of both
  sources, or a session sets the headline while the page that explains the headline cannot
  see it. Name it from the hook's `cwd` when there is no status line payload.
- **A session with no status line is not a session with no numbers.** The hook carries
  `transcript_path`, and every assistant record in a transcript carries a `usage` block, so
  context tokens, model, version and the real project directory are all one tail read away.
  Read the tail only and cache it against size and mtime; this runs on every tick. What is
  genuinely absent is the rate limits: they appear nowhere in the hook payload or the
  transcript, only in the status line.
- **Numbers belong to the session that owns the headline, not to the most recent writer.**
  Context and cost are per session. Not every window mirrors a status line: one running in
  the desktop app fires hooks but never writes one, because that surface draws its own usage
  panel instead of running a status line command. Borrowing the newest other window's
  context and cost and captioning them with this window's name reports one session's work as
  another's. Show the headline session's own numbers, or show none and still name the window.
- **Do not borrow rate limits either.** They are account wide, so a reading from another
  window is not wrong in kind, only as of whenever that window last asked. That distinction
  does not survive being drawn as a live gauge beside a context bar reading "--": the row
  looks current and is not. A headline session with no payload shows nothing at all. The one
  exception is an idle desk, where no session is running and there is nothing for the last
  known numbers to be confused with.
- **Rate limits are account-wide.** They do not vary by session.
- **Prune stale files.** A session killed without `SessionEnd` leaves a `working` or `done`
  file behind. Ignore anything older than ~30 minutes or a dead session pins your display.
- **Rank by wait time, not just state.** A session blocked eight minutes is one you forgot
  about; ten seconds is one you are actively answering. Sort blocked first, longest wait
  first, and the top of your list is always the next thing to do.
- **Separate finished from abandoned.** A session that completed 40 minutes ago is not
  waiting on you, it is over. Treating both as "your turn" turns a to-do list into a
  graveyard. Roughly 30 minutes is a reasonable line.
- **A window silent for hours with no hook activity is closed, not idle.** Drop it.

This is the part worth building, and this repository only reaches the shallow end of it.
"Which of my five agents needs me" is a harder and more useful question than "what is this
one doing".

## Claude API health, if you want it

`https://status.claude.com/api/v2/summary.json`. **Ignore the page-level indicator**: it goes
yellow if any component is degraded, including ones you do not use. Read the component list
and watch only `Claude API (api.anthropic.com)` and `Claude Code`. Map
`operational | degraded_performance | partial_outage | major_outage` to your own levels and
list anything else as a footnote.

## Six states worth distinguishing

This is the design, and it transfers to any medium:

| State | Means | Urgency |
|---|---|---|
| Working | Claude is running | none, but visible |
| Ready | Finished, waiting on you | gentle, persistent |
| Needs you | Permission prompt or question | insistent |
| Rate limited | A window is spent | informational, not urgent |
| API problem | Anthropic-side | red, overlays other states |
| Idle / no link | Nothing happening, or your daemon died | quiet |

**Hue for who is on the hook, motion for urgency.** Colour says whose move it is; movement
says how much you should care. Derive the colour once, in one function, and have every output
read from it. Two hand-maintained lists will drift, and nobody will notice because only one
state is visible at a time.

## Adapt it: replace only the transport

The daemon ends with `send(payload)`. Everything before that is reusable. Three variants worth
building, easiest first:

- **Menu-bar item** (macOS, ~1 hour). A coloured dot plus a percentage. No hardware, no
  network. The highest value-per-effort in this list.
- **Smart bulb / LED strip.** Skip the numbers entirely. Amber when it is your turn is most
  of the value; a lamp that changes colour is read peripherally in a way a screen is not.
- **Phone or tablet as a dashboard.** An old device, a local web page, the daemon serving
  JSON. Big text, no hardware.
- **E-ink badge.** Refresh on state change only. Suits the slow-moving weekly numbers.

Keep: the file layout, the union rule for attention, the six states, the colour-and-motion
split. Discard: everything about SPI, framebuffers and this specific board.

## If you are driving this exact hardware

Traps that each cost hours. Do not rediscover them.

| Symptom | Cause |
|---|---|
| Board never joins Wi-Fi; password is correct | **The ESP32-C6 is 2.4 GHz only.** A 5 GHz-only SSID is invisible to it. Looks exactly like a bad password. |
| Clock shows UTC although NTP succeeded | `configTime(0,0,...)` overwrites the `TZ` env var. Use `configTzTime(TZ_STRING, ...)`. |
| An HTTPS fetch fails silently in a task | The TLS handshake needs a **16 KB stack**. 8 KB fails with no error. |
| Panic when rotating the display | `GFXcanvas16`'s third constructor argument is a **bool, not a buffer**. Re-allocating 110 KB per rotation fails once the radios hold the heap. Subclass it and share one framebuffer. |
| Payloads over ~256 bytes are truncated | Raise `Serial.setRxBufferSize()` before `Serial.begin()`. |
| Display says NO LINK, everything looks fine | Two daemon processes fighting over the port. `pgrep -fl claude_status_daemon`. |
| Port enumerates but nothing responds; flashing fails | The USB stack is wedged. Physical RESET, or hold BOOT while pressing RESET for download mode. Have a network fallback so the device stays useful. |

Build with `PartitionScheme=no_ota`; the radios need the 2 MB app slot.

## Verify against the device, not your assumptions

Add a command that dumps the framebuffer over the wire and a host script that saves it as a
PNG. This turns layout work into something checkable and catches bugs a photograph hides.

It does not catch everything. Two bugs here came only from photographs: a printed case bezel
clipping the right margin, and the LED glow through the case, which is the entire argument for
having an LED at all. Look at the physical object.

## Repository map

| Path | What it is |
|---|---|
| `host/claude_status_daemon.py` | Merge and transport. **Start here.** The reusable part. |
| `host/esp32-status-hook.sh` | The hook. Small, copy it. |
| `host/install.sh` | Wires up the status line patch, hooks and service. |
| `host/screenshot.py` | Framebuffer capture for visual verification. |
| `firmware/claude_status/` | Arduino sketch. Specific to this board; read for ideas, not for porting. |
| `host/gen_sprite.py` | Pixel art via the Gemini API, resampled to RGB565. |
