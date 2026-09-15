#!/bin/bash
# Claude Code hook: record per-session whether Claude is working or waiting on
# the user, for the ESP32 status display daemon. Reads the hook JSON on stdin.
input=$(cat)
IFS=$'\037' read -r ev nt tool agent sid cwd tpath eff < <(printf '%s' "$input" | jq -r '[
  (.hook_event_name // ""), (.notification_type // ""), (.tool_name // ""),
  (.agent_id // ""), (.session_id // "default"), (.cwd // ""),
  (.transcript_path // ""), (.effort.level // "")
] | join("\u001f")')

dir="$HOME/.claude/esp32-status/attention"
mkdir -p "$dir"
file="$dir/$sid.json"

# Subagent activity never changes the headline state (the main session is still working).
case "$ev" in
  Notification)
    case "$nt" in
      permission_prompt|elicitation_dialog) state=needs_input ;;
      idle_prompt) state=done ;;
      *) exit 0 ;;
    esac ;;
  PermissionRequest) state=needs_input ;;
  PreToolUse)
    [[ "$tool" == "AskUserQuestion" ]] || exit 0
    state=needs_input ;;
  Stop) state=done ;;
  UserPromptSubmit|PostToolUse|PostToolUseFailure|PermissionDenied|SessionStart)
    [[ -n "$agent" ]] && exit 0
    state=working ;;
  SessionEnd) rm -f "$file"; exit 0 ;;
  *) exit 0 ;;
esac
tmp=$(mktemp "$dir/$sid.XXXXXX") || exit 0
# jq builds the JSON so a tool or notification name containing a quote cannot corrupt it
# cwd, the transcript path and the effort travel with the state, because a window running in
# the desktop app fires these hooks but never mirrors a status line. The transcript is where
# its token counts and model live, so this is the only route to them.
jq -n --arg state "$state" --arg event "$ev" --arg detail "${nt:-$tool}" --arg cwd "$cwd" \
      --arg transcript "$tpath" --arg effort "$eff" --argjson ts "$(date +%s)" \
      '{state:$state, event:$event, detail:$detail, cwd:$cwd,
        transcript:$transcript, effort:$effort, ts:$ts}' > "$tmp" \
  && mv -f "$tmp" "$file" || rm -f "$tmp"
exit 0
