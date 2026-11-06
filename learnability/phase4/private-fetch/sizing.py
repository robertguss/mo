#!/usr/bin/env python3
"""Independent largest-state fetch sizing; development evidence, never scored evaluation."""
import hashlib
import json
import os
import pathlib
import re
import signal
import shutil
import struct
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "ci"))
from check_cli import Served
from check_operator import SOURCE, TOKEN


def resident(pid):
    text = pathlib.Path(f"/proc/{pid}/status").read_text()
    return {name: int(re.search(rf"^{name}:\s+(\d+)", text, re.M)[1])
            for name in ["VmHWM", "VmRSS"]}



def verify(artifact, state_bytes, binary_sha256):
    assert artifact[:6] == b"MOEX\1\1"
    assert artifact[30:62].hex() == binary_sha256
    assert hashlib.sha256(artifact[:-32]).digest() == artifact[-32:]
    label = struct.unpack_from("<I", artifact, 62)[0]
    saved = artifact[130 + label:-32]
    assert saved[:5] == b"MOSV\1"
    schema_size = struct.unpack_from("<I", saved, 5)[0]
    schema = saved[9:9 + schema_size]
    assert hashlib.sha256(schema).digest() == artifact[94 + label:126 + label]
    # Nominal struct declarations lower to a Choice with one constructor.
    assert schema[0] == 18
    payload = saved[9 + schema_size:]
    assert struct.unpack_from("<II", payload) == (0, 16)
    assert struct.unpack_from("<q", payload, 8)[0] == 42
    assert struct.pack("<I", state_bytes) + b"x" * state_bytes in payload
    if state_bytes == 16767505:
        assert len(saved) == 16777216
    return {"artifact_bytes": len(artifact), "saved_bytes": len(saved),
            "pages": (len(artifact) + 8191) // 8192, "artifact_sha256": artifact[-32:].hex()}


def main():
    binary, output = pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve()
    state_bytes = int(sys.argv[3]) if len(sys.argv) > 3 else 16000000
    assert 0 < state_bytes <= 16777216
    output.parent.mkdir(parents=True, exist_ok=True)
    log_path = output.with_suffix(".fetch.log")
    result = {"binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "fixture_large_text_bytes": state_bytes, "measurement": "one active memory observation"}
    with tempfile.TemporaryDirectory(prefix="mo-fetch-sizing-") as directory:
        root = pathlib.Path(directory)
        (root / "data").mkdir()
        (root / "main.mo").write_text(SOURCE.replace("repeat(100000)", f"repeat({state_bytes})"))
        env = {**os.environ, "OPERATOR_TOKEN": TOKEN}
        service = Served(binary, root / "main.mo", root / "data", env, root)
        child = None
        try:
            assert service.port, service.errors()
            result["service_before_KiB"] = resident(service.process.pid)
            with log_path.open("wb") as log:
                child = subprocess.Popen(["/usr/bin/time", "-f", "fetch_max_RSS_KiB=%M elapsed_seconds=%e",
                    str(binary), "fetch", f"http://127.0.0.1:{service.port}/mcp", "--export", "state",
                    "--out", str(root / "state.moex"), "--token-env", "OPERATOR_TOKEN"],
                    env=env, stdout=log, stderr=log)
                time.sleep(2)
                status, raw = service.post(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                    "params": {"name": "mo-memory", "arguments": {}}}), TOKEN)
                assert status == 200
                result["memory_during"] = json.loads(raw)["result"]["structuredContent"]["value"]
                child.wait(timeout=650)
            result["fetch_status"] = child.returncode
            result["service_after_KiB"] = resident(service.process.pid)
            status, raw = service.post(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                "params": {"name": "mo-memory", "arguments": {}}}), TOKEN)
            assert status == 200
            result["memory_after"] = json.loads(raw)["result"]["structuredContent"]["value"]
            log = log_path.read_text()
            assert TOKEN not in log
            metric = re.search(r"fetch_max_RSS_KiB=(\d+) elapsed_seconds=([\d.]+)", log)
            assert metric
            result["fetch_max_RSS_KiB"] = int(metric[1])
            result["elapsed_seconds"] = float(metric[2])
            target = root / "state.moex"
            result["incomplete_files"] = [p.name for p in root.glob("*.mo-fetch-*")]
            output.write_text(json.dumps(result, indent=2) + "\n")
            if target.exists():
                artifact = target.read_bytes()
                assert target.stat().st_mode & 0o777 == 0o600
                observed = output.with_suffix(".observed.moex")
                descriptor = os.open(observed, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                with os.fdopen(descriptor, "wb") as archive, target.open("rb") as source:
                    shutil.copyfileobj(source, archive)
                result.update(verify(artifact, state_bytes, result["binary_sha256"]))
                assert result["memory_during"]["pinned_export_bytes"] >= len(artifact)
            assert TOKEN not in json.dumps(result)
            output.write_text(json.dumps(result, indent=2) + "\n")
            print(json.dumps(result))
            assert child.returncode == 0 and not result["incomplete_files"], "fetch did not publish"
            assert result["memory_after"]["pinned_export_bytes"] == 0
        finally:
            if child is not None and child.poll() is None:
                child.terminate()
                child.wait(timeout=10)
            if service.process.poll() is None:
                service.finish(0, signal.SIGTERM)
            service.close()


if __name__ == "__main__":
    main()
