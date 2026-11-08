#!/usr/bin/env python3
"""Loopback transport equivalence and rate-limited page retry against a real service."""
import http.client
import http.server
import json
import pathlib
import select
import socket
import socketserver
import sys
import tempfile
import threading

from check_upgrade_live import (Service, V1, V2, V3, TOKEN, UPGRADE, push, write,
                                digests, assert_no_secrets)


class Relay(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


class Forward(socketserver.BaseRequestHandler):
    def handle(self):
        with socket.create_connection(("127.0.0.1", self.server.upstream)) as upstream:
            peers = [self.request, upstream]
            reading = list(peers)
            while reading:
                ready, _, _ = select.select(reading, [], [], 60)
                if not ready:
                    return
                for peer in ready:
                    data = peer.recv(65536)
                    other = peers[1 if peer is peers[0] else 0]
                    if not data:
                        reading.remove(peer)
                        other.shutdown(socket.SHUT_WR)
                    else:
                        other.sendall(data)


class Recording(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        name = json.loads(body).get("params", {}).get("name")
        # Replay the real server's observed no-effect refusal once without sending
        # this page upstream. This stays deterministic even when sanitizer builds
        # process fewer than 20 calls/second and cannot exhaust the real bucket.
        if self.server.throttle and name == "mo-upgrade-page":
            self.server.throttle = False
            self.server.retry_body = body
            error = {"kind": "runtime", "code": "rate_limited",
                     "message": "too many calls; retry later", "effect": "none"}
            structured = {"error": error}
            data = json.dumps({"jsonrpc": "2.0", "id": 1, "result": {
                "content": [{"type": "text", "text": json.dumps(structured,
                             separators=(",", ":"))}],
                "structuredContent": structured, "isError": True}}).encode()
            self.server.record.append({"tool": name, "status": 200,
                                       "rate_limited": True, "forwarded": False})
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if self.server.retry_body is not None:
            assert body == self.server.retry_body, (name, "retry changed the rejected request")
            self.server.retry_body = None
            self.server.retried = True
        connection = http.client.HTTPConnection("127.0.0.1", self.server.upstream, timeout=60)
        headers = {k: v for k, v in self.headers.items() if k.lower() not in
                   ("host", "content-length", "connection")}
        connection.request("POST", "/mcp", body, headers)
        response = connection.getresponse()
        data = response.read()
        # Record routing and outcome only; never request headers, arguments or source.
        self.server.record.append({"tool": name, "status": response.status,
                                   "rate_limited": b'"rate_limited"' in data})
        self.send_response(response.status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        connection.close()


def run(binary):
    binary = str(pathlib.Path(binary).resolve())
    with tempfile.TemporaryDirectory() as root:
        old, new, invalid = (write(root, name, source) for name, source in
                             [("old", V1), ("new", V2), ("invalid", V3)])
        base, candidate = digests(binary, old, new)
        outputs = []
        for mode in ("direct", "recording", "forward", "throttled"):
            data = pathlib.Path(root) / mode
            data.mkdir()
            server = Relay(("127.0.0.1", 0), Forward) if mode == "forward" else None
            flags = ("--allow-host", f"127.0.0.1:{server.server_address[1]}") if server else ()
            service = Service(binary, data, old, flags=flags)
            try:
                url = None
                if mode != "direct":
                    server = server or http.server.ThreadingHTTPServer(("127.0.0.1", 0), Recording)
                    server.upstream, server.service = service.port, service
                    server.record, server.throttle = [], mode == "throttled"
                    server.retry_body, server.retried = None, False
                    thread = threading.Thread(target=server.serve_forever, daemon=True)
                    thread.start()
                    url = f"http://127.0.0.1:{server.server_address[1]}/mcp"
                params = {"name": "mo-upgrade-open", "arguments":
                          {"base": base, "candidate": candidate, "total": 1}}
                status, body, _ = service.rpc("tools/call", params,
                    headers={"Authorization": "Bearer WrongServiceToken012345",
                             "Mo-Upgrade-Authorization": f"Bearer {UPGRADE}"})
                author_status, author_body, _ = service.rpc("tools/call",
                    {"name": "read", "arguments": {}},
                    headers={"Authorization": "Bearer WrongServiceToken012345"})
                assert (status, body) == (author_status, author_body)
                assert status == 401, (status, body)
                for credential in ("", "Bearer WrongUpgradeToken012345"):
                    status, body, _ = service.rpc("tools/call", params,
                        headers={"Mo-Upgrade-Authorization": credential} if credential else {})
                    assert status == 200 and body["result"]["isError"], body
                    assert body["result"]["structuredContent"]["code"] == "E1414", body
                # Successful subsequent open proves neither refusal reserved a slot.
                refused = push(binary, service, invalid, base, url=url)
                assert refused.returncode == 1, refused
                if mode == "throttled":
                    assert server.retried and server.retry_body is None, server.record
                    server.throttle, server.retried = True, False
                accepted = push(binary, service, new, base, url=url)
                assert accepted.returncode == 0, (mode, accepted, server.record if server else [])
                outputs.append((refused.stdout, refused.stderr, accepted.stdout, accepted.stderr))
                assert service.value("read") == "v2:0"
                if mode == "throttled":
                    assert any(row["rate_limited"] for row in server.record), server.record
                    assert server.retried and server.retry_body is None, server.record
                    pages = [row for row in server.record if row["tool"] == "mo-upgrade-page"]
                    assert len(pages) >= 4, pages
                assert_no_secrets(*outputs[-1], json.dumps(server.record if server else []))
            finally:
                if server:
                    server.shutdown()
                    server.server_close()
                    thread.join()
                service.stop()
        assert all(output == outputs[0] for output in outputs), outputs
    print("remote upgrades: direct/proxy/forward reports match; credentials and rate retry passed")


if __name__ == "__main__":
    run(sys.argv[1])
