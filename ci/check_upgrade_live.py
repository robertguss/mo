#!/usr/bin/env python3
"""Live upgrades of a running mo serve: pushes, refusals, streams, kills and recovery.

Every scenario drives the real executable over loopback: mo push for the client path and raw
MCP calls where a test needs to stop between steps."""
import base64
import hashlib
import http.client
import json
import os
import pathlib
import select
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

TOKEN = "LiveServiceToken0123456789"
UPGRADE = "LiveUpgradeCredential9876543210"
ENV = {**os.environ, "TOKEN": TOKEN, "UPGRADE": UPGRADE}

V1 = '''module Main
service
  state Int = 0
  tools add, read, gone, fail_now
  every 40.second tick
  token from env "TOKEN"
end

fn add(state: Int, _now: Time, amount: Int) : (Int, Int)
  (state + amount, state + amount)
end

fn read(state: Int, _now: Time) : Text
  "v1:#{state}"
end

fn gone(state: Int, _now: Time) : (Int, Int)
  (state + 1000, state + 1000)
end

fn fail_now(_state: Int, _now: Time) : (Int, Int)
  stop "the first program fails here"
end

fn tick(state: Int, _now: Time) : Int
  state
end
'''
V2 = '''module Main
service
  state Int = 0
  tools add, read, double
  every 40.second tick
  token from env "TOKEN"
end

fn add(state: Int, _now: Time, amount: Int) : (Int, Int)
  (state + amount, state + amount)
end

fn read(state: Int, _now: Time) : Text
  "v2:#{state}"
end

fn double(state: Int, _now: Time) : (Int, Int)
  (state * 2, state * 2)
end

fn tick(state: Int, _now: Time) : Int
  state
end
'''
V3 = '''module Main
struct Counter
  value: Int
  label: Text
end

service
  state Counter = Counter(value: 0, label: "new")
  tools add, read, double
  every 40.second tick
  token from env "TOKEN"
end

fn migrate(old: Int) : Counter
  Counter(value: old, label: "migrated")
end

fn add(state: Counter, _now: Time, amount: Int) : (Counter, Int)
  next = Counter(value: state.value + amount, label: state.label)
  (next, next.value)
end

fn read(state: Counter, _now: Time) : Text
  "v3:#{state.label}:#{state.value}"
end

fn double(state: Counter, _now: Time) : (Counter, Int)
  next = Counter(value: state.value * 2, label: state.label)
  (next, next.value)
end

fn tick(state: Counter, _now: Time) : Counter
  state
end
'''
# Within the checked-restart budget, beyond the live one.
SLOW = V3.replace('''fn migrate(old: Int) : Counter
  Counter(value: old, label: "migrated")''', '''fn migrate(old: Int) : Counter
  var n = 0
  while n < 200000
    n += 1
  end
  Counter(value: old + n - 200000, label: "slow")''')


def write(folder, name, text):
    path = pathlib.Path(folder) / name / "main.mo"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    return path


def digests(binary, old, new):
    result = subprocess.run([binary, "check", str(new), "--upgrade-from", str(old), "--json"],
                            capture_output=True, timeout=60, env=ENV)
    report = json.loads(result.stdout)
    return report["base_source"], report["candidate_source"]


class Service:
    def __init__(self, binary, data, source=None, flags=(), credential=True):
        args = [binary, "serve", *([str(source)] if source else []), "--data", str(data),
                "--port", "0", *(["--upgrade-token-env", "UPGRADE"] if credential else []), *flags]
        self.errors = tempfile.TemporaryFile()
        self.process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=self.errors,
                                        env=ENV, bufsize=0)
        self.port, self.output = None, b""
        deadline = time.monotonic() + 60
        while self.port is None and time.monotonic() < deadline:
            ready, _, _ = select.select([self.process.stdout], [], [], 1)
            if not ready:
                if self.process.poll() is not None:
                    break
                continue
            chunk = self.process.stdout.read1(4096) if hasattr(self.process.stdout, "read1") \
                else os.read(self.process.stdout.fileno(), 4096)
            if not chunk:
                break
            self.output += chunk
            for line in self.output.decode().splitlines():
                if line.startswith("mo serve: listening on http://127.0.0.1:"):
                    self.port = int(line.rsplit(":", 1)[1].split("/")[0])
        assert self.port is not None, (self.output, self.stderr())

    def stderr(self):
        self.errors.seek(0)
        return self.errors.read().decode(errors="replace")

    def rpc(self, method, params=None, headers=None, upgrade=False):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=60)
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params or {}})
        fields = {"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json",
                  "MCP-Protocol-Version": "2025-11-25", **(headers or {})}
        if upgrade:
            fields["Mo-Upgrade-Authorization"] = f"Bearer {UPGRADE}"
        connection.request("POST", "/mcp", body, fields)
        response = connection.getresponse()
        text = response.read()
        connection.close()
        return response.status, json.loads(text) if text else None, dict(response.getheaders())

    def call(self, tool, upgrade=False, **arguments):
        status, body, _ = self.rpc("tools/call", {"name": tool, "arguments": arguments},
                                   upgrade=upgrade)
        assert status == 200, (status, body)
        return body["result"]

    def value(self, tool, **arguments):
        result = self.call(tool, **arguments)
        assert not result["isError"], result
        return result["structuredContent"]["value"]

    def session(self):
        status, body, headers = self.rpc("initialize", {
            "protocolVersion": "2025-11-25", "capabilities": {},
            "clientInfo": {"name": "check", "version": "1"}})
        assert status == 200 and body["result"]["capabilities"]["tools"]["listChanged"], body
        return headers["MCP-Session-Id"]

    def kill(self):
        self.process.send_signal(signal.SIGKILL)
        self.process.wait(timeout=30)
        self.process.stdout.close()

    def stop(self):
        self.process.send_signal(signal.SIGTERM)
        status = self.process.wait(timeout=60)
        self.process.stdout.close()
        return status


class Stream:
    """A session's GET event stream, read line by line."""

    def __init__(self, port, session):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.socket.sendall((f"GET /mcp HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
                             f"Authorization: Bearer {TOKEN}\r\nAccept: text/event-stream\r\n"
                             f"MCP-Protocol-Version: 2025-11-25\r\nMCP-Session-Id: {session}\r\n"
                             "\r\n").encode())
        self.buffer = b""
        head = self.read_until(b"\r\n\r\n", 10)
        assert head.startswith(b"HTTP/1.1 200 OK\r\n") and b"text/event-stream" in head, head

    def read_until(self, marker, timeout):
        deadline = time.monotonic() + timeout
        while marker not in self.buffer and time.monotonic() < deadline:
            ready, _, _ = select.select([self.socket], [], [], max(0, deadline - time.monotonic()))
            if not ready:
                break
            chunk = self.socket.recv(4096)
            if not chunk:
                break
            self.buffer += chunk
        if marker not in self.buffer:
            return None
        at = self.buffer.index(marker) + len(marker)
        found, self.buffer = self.buffer[:at], self.buffer[at:]
        return found

    def events(self, timeout):
        found = []
        while True:
            event = self.read_until(b"\n\n", timeout)
            if event is None:
                return found
            found.append(json.loads(event.decode().removeprefix("data: ").strip()))
            timeout = 0.2

    def close(self):
        self.socket.close()


def push(binary, service, source, base, *flags, upgrade=UPGRADE, url=None):
    env = {**ENV, "UPGRADE": upgrade}
    return subprocess.run([binary, "push", str(source), "--url",
                           url or f"http://127.0.0.1:{service.port}/mcp", "--base", base,
                           "--token-env", "TOKEN", "--upgrade-token-env", "UPGRADE", *flags],
                          capture_output=True, timeout=120, env=env)


def assert_no_secrets(*texts):
    for text in texts:
        if isinstance(text, bytes):
            text = text.decode(errors="replace")
        assert TOKEN not in text and UPGRADE not in text, text


def folder_bytes(data):
    return b"".join(path.read_bytes() for path in sorted(pathlib.Path(data).iterdir())
                    if path.is_file())


def client_refusals(binary, root):
    source = write(root, "client", V1)
    for url in ["http://example.com:80/mcp", "http://10.1.2.3:8080/mcp", "https://127.0.0.1:1/mcp"]:
        # No credential is set, so a refusal naming the URL proves it came first.
        bare = {k: v for k, v in os.environ.items() if k not in ("TOKEN", "UPGRADE")}
        result = subprocess.run([binary, "push", str(source), "--url", url, "--base", "0" * 64,
                                 "--token-env", "TOKEN", "--upgrade-token-env", "UPGRADE"],
                                capture_output=True, timeout=30, env=bare)
        assert result.returncode == 2 and b"loopback URL" in result.stderr, result


def start_refusals(binary, root):
    data = pathlib.Path(root) / "refusals"
    data.mkdir()
    source = write(root, "refusals-src", V1)
    for value, words in [(TOKEN, b"equals the service token"), ("short", b"shorter than 16")]:
        result = subprocess.run([binary, "serve", str(source), "--data", str(data), "--port", "0",
                                 "--upgrade-token-env", "UPGRADE"], capture_output=True,
                                timeout=60, env={**ENV, "UPGRADE": value})
        assert result.returncode == 1 and words in result.stderr, result
        assert_no_secrets(result.stdout, result.stderr)


def adds(service, stop, acknowledged, errors):
    # About ten calls a second in all, inside the rate limit the push shares with them.
    while not stop.wait(0.4):
        try:
            result = service.call("add", amount="1")
            if result["isError"]:
                errors.append(result)
            else:
                acknowledged.append(1)
        except (OSError, http.client.HTTPException) as error:
            errors.append(repr(error))


def raw_upload(service, source_bytes, base, candidate):
    opened = service.call("mo-upgrade-open", upgrade=True, base=base, candidate=candidate,
                          total=len(source_bytes))
    assert not opened["isError"], opened
    slot = opened["structuredContent"]["slot"]
    for index, offset in enumerate(range(0, len(source_bytes), 8192)):
        page = service.call("mo-upgrade-page", upgrade=True, slot=slot, index=index, offset=offset,
                            total=len(source_bytes),
                            bytes=base64.b64encode(source_bytes[offset:offset + 8192]).decode())
        assert not page["isError"], page
    return slot


def bundle_bytes(source):
    """The canonical one-file MOSB v1 bundle: entry path, then (path, source) pairs."""
    text = pathlib.Path(source).read_bytes()
    entry = b"main.mo"
    return (b"MOSB\x01" + len(entry).to_bytes(4, "little") + entry + (1).to_bytes(4, "little") +
            len(entry).to_bytes(4, "little") + entry + len(text).to_bytes(4, "little") + text)


def main_flow(binary, root):
    data = pathlib.Path(root) / "main"
    data.mkdir()
    v1, v2, v3, slow = (write(root, name, text) for name, text in
                        [("v1", V1), ("v2", V2), ("v3", V3), ("slow", SLOW)])
    base1, digest2 = digests(binary, v1, v2)
    _, digest3 = digests(binary, v2, v3)
    service = Service(binary, data, v1)
    outputs = []
    try:
        first, second = service.session(), service.session()
        streams = [Stream(service.port, first), Stream(service.port, second)]
        failed = service.call("fail_now")
        assert failed["isError"], failed
        records = service.call("mo-failures")["structuredContent"]["value"]["records"]
        record = records[0]["id"]
        timers = service.call("mo-timers")["structuredContent"]["value"]
        # The service token alone and a wrong credential change nothing and send no notice.
        wrong = push(binary, service, v2, base1, upgrade="NotTheUpgradeCredential0")
        outputs += [wrong.stdout, wrong.stderr]
        assert wrong.returncode == 1 and b"E1414" in wrong.stderr, wrong
        # A wrong service token never reaches the tools: nothing changed, and --json stays JSON.
        rejected = subprocess.run([binary, "push", str(v2), "--url",
                                   f"http://127.0.0.1:{service.port}/mcp", "--base", base1,
                                   "--token-env", "TOKEN", "--upgrade-token-env", "UPGRADE",
                                   "--json"], capture_output=True, timeout=120,
                                  env={**ENV, "TOKEN": "WrongServiceToken0123456"})
        report = json.loads(rejected.stdout)
        assert rejected.returncode == 1 and report["class"] == "client", rejected
        assert "nothing was changed" in report["message"], report
        outputs += [rejected.stdout, rejected.stderr]
        assert all(stream.events(0.5) == [] for stream in streams)
        assert service.value("read") == "v1:0"
        # A push under concurrent callers: every acknowledged call lands exactly once.
        stop, acknowledged, errors = threading.Event(), [], []
        workers = [threading.Thread(target=adds, args=(service, stop, acknowledged, errors))
                   for _ in range(4)]
        for worker in workers:
            worker.start()
        time.sleep(1)
        accepted = push(binary, service, v2, base1, "--json")
        time.sleep(1)
        stop.set()
        for worker in workers:
            worker.join()
        outputs += [accepted.stdout, accepted.stderr]
        assert accepted.returncode == 0, accepted
        report = json.loads(accepted.stdout)
        assert report["status"] == "accepted" and report["ordinal"] == "1", report
        assert report["candidate_source"] == digest2 and report["active_source"] == digest2
        assert {"kind": "tool", "name": "double", "change": "added"} in report["differences"]
        assert {"kind": "tool", "name": "gone", "change": "removed"} in report["differences"]
        assert errors == [], errors
        assert service.value("read") == f"v2:{len(acknowledged)}", (len(acknowledged))
        # Each session's one stream heard of the new tool list once, after the commit.
        for stream in streams:
            notices = stream.events(5)
            assert notices == [{"jsonrpc": "2.0", "method": "notifications/tools/list_changed"}]
        status, body, _ = service.rpc("tools/list", headers={"MCP-Session-Id": first})
        names = [tool["name"] for tool in body["result"]["tools"]]
        assert "double" in names and "gone" not in names and "mo-upgrade-commit" in names, names
        status, body, _ = service.rpc("tools/call", {"name": "gone"})
        assert body["error"]["message"] == "Invalid params: unknown tool", body
        # Timer phase is kept by name: the next due time did not move.
        after = service.call("mo-timers")["structuredContent"]["value"]
        assert after["items"][0]["next_due_elapsed_us"] == timers["items"][0]["next_due_elapsed_us"], (timers, after)
        # Earlier events keep their program's handler names; the upgrade is an event.
        events = service.call("mo-supervisor")["structuredContent"]["value"]["items"]
        kinds = {event["kind"] for event in events}
        assert "upgrade" in kinds, events
        assert any(e["kind"] == "failure" and e["handler"] == "fail_now" for e in events), events
        assert any(e["kind"] == "upgrade" and e["reason"].startswith("accepted ordinal 1")
                   for e in events), events
        assert any(e["kind"] == "upgrade" and e["reason"].startswith("refused E1414")
                   for e in events), events
        # Retrying the push that already landed, and a push against a stale base.
        again = push(binary, service, v2, base1)
        assert again.returncode == 1 and b"E1416" in again.stderr and digest2.encode() in again.stderr
        same = push(binary, service, v2, digest2)
        assert same.returncode == 0 and b"already active" in same.stdout, same
        # A migration over the live budget is refused; the old program keeps serving.
        refused = push(binary, service, slow, digest2, "--migrate", "migrate", "--json")
        report = json.loads(refused.stdout)
        assert refused.returncode == 1 and report["code"] == "E1424", report
        assert service.value("read").startswith("v2:")
        assert all(stream.events(0.3) == [] for stream in streams)
        counted = len(acknowledged)
        migrated = push(binary, service, v3, digest2, "--migrate", "migrate", "--json")
        report = json.loads(migrated.stdout)
        assert migrated.returncode == 0 and report["ordinal"] == "2", report
        assert int(report["vm_work"]) > 0 and report["migration"] == "migrate", report
        assert service.value("read") == f"v3:migrated:{counted}"
        for stream in streams:
            stream.close()
        outputs += [migrated.stdout, migrated.stderr]
        # Killed after the commit: the restart runs the recorded program without a source file.
        service.kill()
        service = Service(binary, data)
        assert service.value("read") == f"v3:migrated:{counted}"
        assert service.value("add", amount="2") == str(counted + 2)
        same = push(binary, service, v3, digest3, "--migrate", "migrate")
        assert same.returncode == 0 and b"already active" in same.stdout, same
        stale = push(binary, service, v3, digest2, "--migrate", "migrate")
        assert stale.returncode == 1 and digest3.encode() in stale.stderr, stale
        # A second serve on the folder waits for the lock and exits busy without serving.
        started = time.monotonic()
        busy = subprocess.run([binary, "serve", "--data", str(data), "--port", "0"],
                              capture_output=True, timeout=60, env=ENV)
        assert busy.returncode == 75 and b"stopped waiting" in busy.stderr, busy
        assert time.monotonic() - started >= 9
        logs = service.stderr()
    finally:
        if service.process.poll() is None:
            service.stop()
    assert_no_secrets(logs, *outputs)
    assert TOKEN.encode() not in folder_bytes(data) and UPGRADE.encode() not in folder_bytes(data)
    history = subprocess.run([binary, "history", str(data), "--json"], capture_output=True,
                             timeout=60, env=ENV)
    assert history.returncode == 0, history
    text = history.stdout.decode()
    assert '"ordinal":"1"' in text or '"ordinal":1' in text, text[:2000]
    # The first program's failure replays from the program the folder retained for it.
    replay = subprocess.run([binary, "replay", "--failure", str(record), "--data", str(data)],
                            capture_output=True, timeout=120, env=ENV)
    assert b"fail_now" in replay.stdout + replay.stderr, replay
    assert b"no retained program" not in replay.stderr, replay


def staged_kill(binary, root):
    data = pathlib.Path(root) / "staged"
    data.mkdir()
    v1, v2 = write(root, "s1", V1), write(root, "s2", V2)
    base1, digest2 = digests(binary, v1, v2)
    candidate = bundle_bytes(v2)
    assert hashlib.sha256(candidate).hexdigest() == digest2
    service = Service(binary, data, v1)
    assert service.value("add", amount="5") == "5"
    raw_upload(service, candidate, base1, digest2)
    time.sleep(2)
    service.kill()
    service = Service(binary, data)
    try:
        assert service.value("read") == "v1:5"
        accepted = push(binary, service, v2, base1)
        assert accepted.returncode == 0, accepted
        assert service.value("read") == "v2:5"
    finally:
        service.stop()


def budget_then_restart(binary, root):
    data = pathlib.Path(root) / "budget"
    data.mkdir()
    v1, slow = write(root, "b1", V1), write(root, "b2", SLOW)
    base1, _ = digests(binary, v1, v1)
    service = Service(binary, data, v1)
    try:
        assert service.value("add", amount="4") == "4"
        refused = push(binary, service, slow, base1, "--migrate", "migrate", "--json")
        assert refused.returncode == 1 and json.loads(refused.stdout)["code"] == "E1424"
        assert service.value("read") == "v1:4"
    finally:
        service.stop()
    service = Service(binary, data, slow, ["--upgrade", "--migrate", "migrate"])
    try:
        assert service.value("read") == "v3:slow:4"
    finally:
        service.stop()


def back_to_back(binary, root):
    data = pathlib.Path(root) / "twice"
    data.mkdir()
    v1, v2, v3 = write(root, "t1", V1), write(root, "t2", V2), write(root, "t3", V3)
    base1, digest2 = digests(binary, v1, v2)
    service = Service(binary, data, v1)
    try:
        first = push(binary, service, v2, base1, "--json")
        second = push(binary, service, v3, digest2, "--migrate", "migrate", "--json")
        a, b = json.loads(first.stdout), json.loads(second.stdout)
        assert (a["ordinal"], b["ordinal"]) == ("1", "2") and a["head"] == b["head"], (a, b)
        assert service.value("read") == "v3:migrated:0"
    finally:
        service.stop()
    history = subprocess.run([binary, "history", str(data)], capture_output=True, timeout=60,
                             env=ENV)
    text = history.stdout.decode()
    assert history.returncode == 0 and text.count("upgrade ") >= 2, text


def main(binary):
    binary = str(pathlib.Path(binary).resolve())
    with tempfile.TemporaryDirectory() as root:
        client_refusals(binary, root)
        start_refusals(binary, root)
        main_flow(binary, root)
        staged_kill(binary, root)
        budget_then_restart(binary, root)
        back_to_back(binary, root)
    print("live upgrades: pushes, refusals, streams, timers, events, kills, restarts, "
          "retained replay and credential hygiene passed")


if __name__ == "__main__":
    main(sys.argv[1])
