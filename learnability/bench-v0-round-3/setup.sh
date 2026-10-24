#!/bin/sh
# Starts a benchmark v0 session through the harness (learnability/harness/README.md), then
# gives its work folder the task's start files (bench/v0/README.md, "What a session gets").
# The session folder sits inside the workspace, beside mo-work, so it can be collected.
set -e
F=/home/user/.mo-frozen
python3 "$F/session.py" start --mo "$F/mo" --sha256 FROZEN_SHA256 \
    --work /home/user/workspace/mo-work /home/user/workspace/mo-session > /dev/null
cp -R "$F/start/." /home/user/workspace/mo-work/
mkdir -p /home/user/.local/bin
ln -sf /home/user/workspace/mo-session/bin/mo /home/user/.local/bin/mo
echo ready
