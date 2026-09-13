#!/bin/bash
# Claude Code hook: record per-session whether Claude is working or waiting on
# the user, for the ESP32 status display daemon. Reads the hook JSON on stdin.
input=$(cat)
IFS=$'\037' read -r ev nt tool agent sid < <(printf '%s' "$input" | jq -r '[
  (.hook_event_name // ""), (.notification_type // ""), (.tool_name // ""),
  (.agent_id // ""), (.session_id // "default")
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
jq -n --arg state "$state" --arg event "$ev" --arg detail "${nt:-$tool}" \
      --argjson ts "$(date +%s)" '{state:$state, event:$event, detail:$detail, ts:$ts}' > "$tmp" \
  && mv -f "$tmp" "$file" || rm -f "$tmp"
exit 0
