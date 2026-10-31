#!/usr/bin/env bash
set +x
trap 'exit 0' EXIT

unset MO_WEBHOOK_URL MO_WEBHOOK_HEADER
env_file="$HOME/.config/mo/webhook.env"
if [ -f "$env_file" ]; then
	. "$env_file" >/dev/null 2>&1
fi
if [ -z "${MO_WEBHOOK_URL:-}" ] || [ -z "${MO_WEBHOOK_HEADER:-}" ]; then
	exit 0
fi

payload=$(python3 -I -c '
import json, os, subprocess, sys

try:
    hook = {} if "--test" in sys.argv else json.load(sys.stdin)
except ValueError:
    hook = {}
cwd = hook.get("cwd") or os.getcwd()
branch = subprocess.run(
    ["git", "-C", cwd, "branch", "--show-current"], capture_output=True, text=True
).stdout.strip()
payload = {
    "cwd": cwd,
    "branch": branch,
    "session_id": hook.get("session_id", ""),
    "repo": "robertguss/mo",
    "event": "stop",
}
if "--test" in sys.argv:
    payload["test"] = True
print(json.dumps(payload))
' "$@" 2>/dev/null) || exit 0

curl -q --silent --globoff --output /dev/null --max-time 5 --request POST \
	--header "Content-Type: application/json" \
	--header @<(printf '%s\n' "$MO_WEBHOOK_HEADER") \
	--data-binary "$payload" --url "$MO_WEBHOOK_URL" >/dev/null 2>&1
