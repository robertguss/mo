#!/usr/bin/env python3
"""Native-client discovery and host-local fetch smoke, not effectiveness evaluation."""
import importlib.util
import pathlib
import shlex
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "learnability/phase4/preflight"))
import client_probe as preflight

spec = importlib.util.spec_from_file_location("live_views_probe", ROOT / "learnability/phase4/live-views/client_probe.py")
views = importlib.util.module_from_spec(spec)
spec.loader.exec_module(views)
# Importing the existing probe installs its own model wrapper; reuse its underlying runner.
run_process = views.original_run
preflight.proxy_handler = views.forwarding
binary = pathlib.Path(sys.argv[1]).resolve()
output = pathlib.Path(sys.argv[2]).resolve()


def run(command, workdir, out):
    command[command.index("read-only")] = "workspace-write"
    command[2:2] = ["--model", "gpt-6.1-sol", "-c", 'model_reasoning_effort="high"',
                   "-c", "sandbox_workspace_write.network_access=true"]
    return run_process(command, workdir, out)


original_codex = preflight.run_codex


def client(url, workdir, out, approve=False):
    command = shlex.join([str(binary), "fetch", url, "--export", "state", "--out",
                          str(workdir / "state.moex"), "--token-env", "PROBE_TOKEN"])
    preflight.PROMPT = (
        "Development smoke, not effectiveness evaluation. Discover the connected probe MCP tools. "
        "Call mo-overview and mo-export with kind state. Do not call author handlers. "
        f"Then use your shell tool to run this exact command: {command}. "
        "Read the published file locally and verify mode 0600 and magic MOEX. "
        "Do not print or inspect the token, print state, or edit source. "
        "Never read or copy /home/exedev/mo-phase4-private. "
        "Report every tool name as a fully qualified mcp__probe__NAME separately (no grouped prefix), "
        "fetch exit status, file mode and magic. Creation, final-page release and publication are "
        "separate events; this fetch starts a new generation."
    )
    return original_codex(url, workdir, out, approve=True)


preflight.run = run
preflight.run_codex = client
if __name__ == "__main__":
    sys.exit(preflight.main([str(binary), str(output), "codex"]))
