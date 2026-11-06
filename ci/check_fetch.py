#!/usr/bin/env python3
"""Private fetch through real loopback sockets and independent artifact checks."""
import hashlib
import base64
import http.server
import json
import socket
import threading
import time
import os
import pathlib
import signal
import struct
import subprocess
import sys
import tempfile

from check_cli import Served
from check_operator import FAILING, SOURCE, TOKEN, body, client, exported
from check_export import SOURCE as COMMIT_SOURCE



def commits(binary):
    with tempfile.TemporaryDirectory(prefix="mo-fetch-commits-") as directory:
        root = pathlib.Path(directory)
        (root / "main.mo").write_text(COMMIT_SOURCE)
        (root / "data").mkdir()
        env = {**os.environ, "OPERATOR_TOKEN": TOKEN}
        service = Served(binary, root / "main.mo", root / "data", env, root)
        generations = []

        class Relay(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                request = self.rfile.read(int(self.headers["Content-Length"]))
                status, raw = service.post(request.decode(), TOKEN)
                page = json.loads(raw)["result"]["structuredContent"]["value"]
                generations.append(page["export"])
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)
                if page["offset"] == 0:
                    status, _ = service.post(json.dumps({"jsonrpc": "2.0", "id": 1,
                        "method": "tools/call", "params": {"name": "change", "arguments": {}}}), TOKEN)
                    assert status == 200

        relay = http.server.HTTPServer(("127.0.0.1", 0), Relay)
        worker = threading.Thread(target=relay.serve_forever)
        worker.start()
        try:
            status, raw = service.post(json.dumps({"jsonrpc": "2.0", "id": 1,
                "method": "tools/call", "params": {"name": "mo-overview", "arguments": {}}}), TOKEN)
            assert status == 200
            overview = json.loads(raw)["result"]["structuredContent"]["value"]
            time.sleep(0.055)
            for name in ["first", "retry"]:
                result = subprocess.run([str(binary), "fetch", f"http://127.0.0.1:{relay.server_port}/mcp",
                    "--export", "state", "--out", str(root / name), "--token-env", "OPERATOR_TOKEN"],
                    env=env, capture_output=True, timeout=30)
                assert result.returncode == 0, result.stderr
                assert (root / name).stat().st_mode & 0o777 == 0o600
            first, retry = (root / "first").read_bytes(), (root / "retry").read_bytes()
            assert first[6:22] != retry[6:22]
            for artifact, expected, sequence in [(first, b"a" * 20000, 0),
                                                  (retry, b"a" * 20000 + b"b", 1)]:
                assert artifact[:6] == b"MOEX\1\1"
                assert struct.unpack_from("<Q", artifact, 22)[0] == int(overview["program_identity"])
                assert artifact[30:62] == hashlib.sha256(binary.read_bytes()).digest()
                assert hashlib.sha256(artifact[:-32]).digest() == artifact[-32:]
                label = struct.unpack_from("<I", artifact, 62)[0]
                assert artifact[66:66 + label].decode() == overview["runtime_build"]
                service_id, durable, encoded, created = struct.unpack_from("<IQQq", artifact, 66 + label)
                assert service_id == 0 and durable == encoded == sequence and created > 0
                saved = artifact[130 + label:-32]
                assert len(saved) == struct.unpack_from("<I", artifact, 126 + label)[0]
                assert saved[:5] == b"MOSV\1"
                schema_size = struct.unpack_from("<I", saved, 5)[0]
                # Saved-state spec: Text tag, zero parameters, one group, zero fields.
                expected_schema = struct.pack("<BIII", 10, 0, 1, 0)
                assert saved[9:9 + schema_size] == expected_schema
                assert artifact[94 + label:126 + label] == hashlib.sha256(expected_schema).digest()
                payload = saved[9 + schema_size:]
                assert payload == struct.pack("<I", len(expected)) + expected
            assert len(generations) == 6 and len(set(generations[:3])) == len(set(generations[3:])) == 1
            assert first[6:22].hex() == generations[0] and retry[6:22].hex() == generations[3]
            assert generations[0] != generations[3]
        finally:
            relay.shutdown()
            worker.join()
            relay.server_close()
            service.finish(0, signal.SIGTERM)
            service.close()



def lifecycle(binary, mode):
    with tempfile.TemporaryDirectory(prefix="mo-fetch-lifecycle-") as directory:
        root = pathlib.Path(directory)
        (root / "main.mo").write_text(COMMIT_SOURCE)
        (root / "data").mkdir()
        env = {**os.environ, "OPERATOR_TOKEN": TOKEN}
        service = Served(binary, root / "main.mo", root / "data", env, root)
        first_id = []
        second = threading.Event()
        intervened = False

        class Relay(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                nonlocal intervened
                request = self.rfile.read(int(self.headers["Content-Length"]))
                status, raw = service.post(request.decode(), TOKEN)
                value = json.loads(raw)["result"]["structuredContent"]["value"]
                if not intervened and "offset" in value and value["offset"] == 0:
                    first_id.append(value["export"])
                    intervened = True
                    args = {"kind": "state"}
                    if mode == "release":
                        args.update(export=value["export"], offset=((value["total"] - 1) // 8192) * 8192)
                    if mode in ["replace", "release"]:
                        code, _ = service.post(json.dumps({"jsonrpc": "2.0", "id": 1,
                            "method": "tools/call", "params": {"name": "mo-export", "arguments": args}}), TOKEN)
                        assert code == 200
                    if mode == "expiry":
                        time.sleep(60.1)
                elif mode == "corrupt" and value.get("offset") == 8192:
                    value["base64"] = ("A" if value["base64"][0] != "A" else "B") + value["base64"][1:]
                    changed = json.loads(raw)
                    changed["result"]["structuredContent"]["value"] = value
                    raw = json.dumps(changed, separators=(",", ":")).encode()
                elif mode == "corrupt-digest" and value.get("next", False) is None:
                    value["digest"] = ("0" if value["digest"][0] != "0" else "1") + value["digest"][1:]
                    changed = json.loads(raw)
                    changed["result"]["structuredContent"]["value"] = value
                    raw = json.dumps(changed, separators=(",", ":")).encode()
                elif mode == "cancel" and "offset" in value:
                    second.set()
                    time.sleep(0.5)
                try:
                    self.send_response(status)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(raw)))
                    self.end_headers()
                    self.wfile.write(raw)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        relay = http.server.HTTPServer(("127.0.0.1", 0), Relay)
        worker = threading.Thread(target=relay.serve_forever)
        worker.start()
        try:
            target = root / "refused"
            command = [str(binary), "fetch", f"http://127.0.0.1:{relay.server_port}/mcp",
                "--export", "state", "--out", str(target), "--token-env", "OPERATOR_TOKEN"]
            process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if mode == "cancel":
                assert second.wait(15)
                process.send_signal(signal.SIGTERM)
            stdout, stderr = process.communicate(timeout=100)
            assert process.returncode == 1 and not target.exists(), (mode, stderr)
            assert TOKEN.encode() not in stdout + stderr
            if mode == "replace":
                assert b"export replaced" in stderr
            if mode in ["expiry", "release"]:
                assert b"released or expired" in stderr
            assert first_id and not list(root.glob("*.mo-fetch-*"))
            retry = subprocess.run([str(binary), "fetch", f"http://127.0.0.1:{service.port}/mcp",
                "--export", "state", "--out", str(root / "retry"), "--token-env", "OPERATOR_TOKEN"],
                env=env, capture_output=True, timeout=15)
            assert retry.returncode == 0, retry.stderr
            assert (root / "retry").read_bytes()[6:22].hex() != first_id[0]
        finally:
            relay.shutdown()
            worker.join()
            relay.server_close()
            service.finish(0, signal.SIGTERM)
            service.close()


def failure(binary):
    with tempfile.TemporaryDirectory(prefix="mo-fetch-failure-") as directory:
        root = pathlib.Path(directory)
        (root / "main.mo").write_text(FAILING)
        (root / "data").mkdir()
        env = {**os.environ, "OPERATOR_TOKEN": TOKEN}
        service = Served(binary, root / "main.mo", root / "data", env, root)
        try:
            rpc, call = client(service)
            status, reply = rpc("tools/call", {"name": "broken", "arguments": {"_n": "7"}})
            assert status == 200 and reply["result"]["isError"] is True, reply
            overview = call("mo-overview")
            assert overview["retained_failures"] == 1, overview

            def fetch(target, *selector):
                time.sleep(0.055)
                result = subprocess.run([str(binary), "fetch", f"http://127.0.0.1:{service.port}/mcp",
                    "--export", *selector, "--out", str(target), "--token-env", "OPERATOR_TOKEN"],
                    env=env, capture_output=True, umask=0o777, timeout=30)
                assert TOKEN.encode() not in result.stdout + result.stderr
                assert not list(root.glob("*.mo-fetch-*"))
                return result

            missing = fetch(root / "missing", "failure", "2")
            assert missing.returncode == 1 and not (root / "missing").exists(), missing.stderr
            target = root / "failure.moex"
            result = fetch(target, "failure", "1")
            assert result.returncode == 0 and not result.stdout, result.stderr
            assert target.stat().st_mode & 0o777 == 0o600
            artifact = target.read_bytes()
            assert artifact[:6] == b"MOEX\1\2"
            assert hashlib.sha256(artifact[:-32]).digest() == artifact[-32:]
            assert struct.unpack_from("<Q", artifact, 22)[0] == int(overview["program_identity"])
            assert artifact[30:62] == hashlib.sha256(binary.read_bytes()).digest()
            label = struct.unpack_from("<I", artifact, 62)[0]
            assert artifact[66:66 + label].decode() == overview["runtime_build"]
            assert struct.unpack_from("<IQQ", artifact, 66 + label) == (0, 0, 0)
            direct = exported(call, call("mo-export", kind="failure", failure=1))
            assert direct[6:22] != artifact[6:22] and body(direct) == body(artifact)
            assert b"x" * 20000 in body(artifact)
            assert fetch(target, "failure", "1").returncode == 1 and target.read_bytes() == artifact
            state = root / "state.moex"
            assert fetch(state, "state").returncode == 0 and state.read_bytes()[:6] == b"MOEX\1\1"
            assert service.finish(0, signal.SIGTERM)
        finally:
            service.close()


def run(binary):
    binary = pathlib.Path(binary).resolve()
    failure(binary)
    with tempfile.TemporaryDirectory(prefix="mo-fetch-") as directory:
        root = pathlib.Path(directory)
        source = root / "main.mo"
        source.write_text(SOURCE)
        data = root / "data"
        data.mkdir()
        env = {**os.environ, "OPERATOR_TOKEN": TOKEN,
               "http_proxy": "http://192.0.2.1:1", "HTTP_PROXY": "http://192.0.2.1:1"}
        service = Served(binary, source, data, env, root)
        try:
            assert service.port, service.errors()
            url = f"http://127.0.0.1:{service.port}/mcp"

            def fetch(target, token=TOKEN, address=url, umask=-1):
                result = subprocess.run([str(binary), "fetch", address, "--export", "state",
                                         "--out", str(target), "--token-env", "OPERATOR_TOKEN"],
                                        env={**env, "OPERATOR_TOKEN": token}, capture_output=True, umask=umask,
                                        timeout=30)
                assert TOKEN.encode() not in result.stdout + result.stderr
                assert not list(root.glob("*.mo-fetch-*"))
                return result

            target = root / "artifact"
            result = fetch(target, umask=0o777)
            assert result.returncode == 0, result.stderr
            assert target.stat().st_mode & 0o777 == 0o600
            artifact = target.read_bytes()
            assert artifact[:6] == b"MOEX\1\1"
            assert artifact[30:62] == hashlib.sha256(binary.read_bytes()).digest()
            assert hashlib.sha256(artifact[:-32]).digest() == artifact[-32:]
            label = struct.unpack_from("<I", artifact, 62)[0]
            at = 66 + label
            sequence, encoded = struct.unpack_from("<QQ", artifact, at + 4)
            assert sequence == encoded == 1
            schema = artifact[at + 28:at + 60]
            size = struct.unpack_from("<I", artifact, at + 60)[0]
            saved = artifact[at + 64:-32]
            assert len(saved) == size and saved[:5] == b"MOSV\1"
            schema_size = struct.unpack_from("<I", saved, 5)[0]
            assert hashlib.sha256(saved[9:9 + schema_size]).digest() == schema
            assert b"private \xc3\xa9" in saved and b"x" * 100000 in saved
            assert fetch(target).returncode == 1 and target.read_bytes() == artifact
            permissive = root / "permissive"
            permissive.write_bytes(b"keep")
            permissive.chmod(0o666)
            assert fetch(permissive).returncode == 1
            assert permissive.read_bytes() == b"keep" and permissive.stat().st_mode & 0o777 == 0o666
            symlink = root / "symlink"
            symlink.symlink_to(root / "missing")
            assert fetch(symlink).returncode == 1 and symlink.is_symlink()
            assert fetch(root / "wrong", "wrong").returncode == 1
            assert not (root / "wrong").exists()
            for address in ["http://192.0.2.1:80/mcp", "https://localhost:80/mcp",
                            "http://user@localhost:80/mcp", "http://localhost:x/mcp"]:
                assert fetch(root / "bad", address=address).returncode == 2
                assert not (root / "bad").exists()
            # Relay actual pages and alter only metadata/checksum, retaining every refusal.
            def altered_page(key, value):
                request = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                    "params": {"name": "mo-export", "arguments": {"kind": "state"}}})
                status, raw = service.post(request, TOKEN)
                assert status == 200
                body = json.loads(raw)
                page = body["result"]["structuredContent"]["value"]
                page[key] = value
                if key == "offset" and page["next"] is not None and isinstance(value, int):
                    page["next"]["offset"] = value + page["length"]
                payload = json.dumps(body, separators=(",", ":")).encode()
                return b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + str(len(payload)).encode() + b"\r\n\r\n" + payload

            # Adversarial closed responses are served over actual loopback sockets.
            def bad_response(wire, cancel=False, accepted=False, sequences=(9, 8), host="127.0.0.1"):
                listener = socket.socket(socket.AF_INET6 if host == "::1" else socket.AF_INET)
                listener.bind(("127.0.0.1" if host == "localhost" else host, 0))
                listener.listen()
                port = listener.getsockname()[1]
                address = f"http://{'[::1]' if host == '::1' else host}:{port}/mcp"
                requests = []
                done = threading.Event()
                def serve():
                    connection, _ = listener.accept()
                    with connection:
                        requests.append(connection.recv(4096))
                        if cancel:
                            time.sleep(0.5)
                        else:
                            connection.sendall(wire)
                    listener.settimeout(0.05)
                    while not done.is_set():
                        try:
                            extra, _ = listener.accept()
                            with extra:
                                requests.append(extra.recv(4096))
                        except TimeoutError:
                            pass
                    listener.close()
                worker = threading.Thread(target=serve, daemon=True)
                worker.start()
                target = root / "refused"
                if cancel:
                    process = subprocess.Popen([str(binary), "fetch", address,
                        "--export", "state", "--out", str(target), "--token-env", "OPERATOR_TOKEN"],
                        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    deadline = time.monotonic() + 10
                    while not requests and time.monotonic() < deadline:
                        time.sleep(0.01)
                    assert requests
                    process.send_signal(signal.SIGTERM)
                    _, stderr = process.communicate(timeout=5)
                    assert process.returncode == 1 and TOKEN.encode() not in stderr
                else:
                    assert fetch(target, address=address).returncode == (0 if accepted else 1)
                done.set()
                worker.join(5)
                assert not worker.is_alive() and len(requests) == 1 and target.exists() == accepted
                if accepted:
                    artifact = target.read_bytes()
                    value = json.loads(wire.split(b"\r\n\r\n", 1)[1])["result"]["structuredContent"]["value"]
                    assert artifact == base64.b64decode(value["base64"], validate=True)
                    label = struct.unpack_from("<I", artifact, 62)[0]
                    assert struct.unpack_from("<QQ", artifact, 70 + label) == sequences
                    assert target.stat().st_mode & 0o777 == 0o600
                    target.unlink()
                assert not list(root.glob("*.mo-fetch-*"))
            fixture = (pathlib.Path(__file__).resolve().parents[1] / "fuzz/corpus/fuzz_fetch/page").read_bytes()
            for host in ["127.0.0.1", "localhost", "::1"]:
                bad_response(fixture, accepted=True, host=host)
            # Encoded state may be ahead of the durable sequence after snapshot failure.
            envelope = json.loads(fixture.split(b"\r\n\r\n", 1)[1])
            value = envelope["result"]["structuredContent"]["value"]
            raw = bytearray(base64.b64decode(value["base64"], validate=True))
            label = struct.unpack_from("<I", raw, 62)[0]
            struct.pack_into("<Q", raw, 78 + label, 10)
            raw[-32:] = hashlib.sha256(raw[:-32]).digest()
            value["encoded_sequence"] = "10"
            value["base64"] = base64.b64encode(raw).decode()
            value["digest"] = raw[-32:].hex()
            checksum = 14695981039346656037
            for byte in raw:
                checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
            value["checksum"] = f"{checksum:016x}"
            payload = json.dumps(envelope, separators=(",", ":")).encode()
            response = b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + str(len(payload)).encode() + b"\r\n\r\n" + payload
            bad_response(response, accepted=True, sequences=(9, 10))
            for wire in [
                b"HTTP/1.1 302 Found\r\nLocation: http://192.0.2.1:1/mcp\r\nContent-Length: 0\r\n\r\n",
                b"HTTP/1.1 200 OK\r\nContent-Length: 99\r\nContent-Type: application/json\r\n\r\n{}",
                b"HTTP/1.1 200 OK\r\nX-Pad: " + b"x" * 1100 + b"\r\n\r\n",
                b"HTTP/1.1 200 OK\r\nContent-Length: 32769\r\n\r\n" + b"x" * 32769,
                b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Type: application/json\r\n\r\n{}",
            ]:
                bad_response(wire)
            for key, value in [("export", "wrong"), ("checksum", "0000000000000000"),
                               ("offset", 8192), ("total", 999999999), ("length", 1),
                               ("pages", 1), ("base64", "!!!!"), ("version", 2),
                               ("sequence", 1), ("offset", "0"), ("version", "1")]:
                time.sleep(0.055)
                bad_response(altered_page(key, value))
            bad_response(b"", cancel=True)
            assert service.finish(0, signal.SIGTERM)
        finally:
            service.close()
    commits(binary)
    for mode in ["replace", "release", "cancel", "corrupt", "corrupt-digest"]:
        lifecycle(binary, mode)
    print("fetch: private state and failure artifacts, exact state during commits, identities/digests, lifecycle, proxy bypass and refusals passed")


if __name__ == "__main__":
    run(sys.argv[1])
    if "--expiry" in sys.argv[2:]:
        lifecycle(pathlib.Path(sys.argv[1]).resolve(), "expiry")
        print("fetch: actual idle expiry and fresh retry passed")
