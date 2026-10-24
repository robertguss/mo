#!/bin/sh
# Starts a final-round session through the harness (learnability/harness/README.md).
# The session folder sits inside the workspace, beside mo-work, so it can be collected
# with Amp's file download (diagnostic round, finding 5).
set -e
F=/home/user/.mo-frozen
python3 "$F/session.py" start --mo "$F/mo" --sha256 FROZEN_SHA256 \
    --work /home/user/workspace/mo-work /home/user/workspace/mo-session > /dev/null
mkdir -p /home/user/.local/bin
ln -sf /home/user/workspace/mo-session/bin/mo /home/user/.local/bin/mo
echo ready
