#!/usr/bin/env python3
"""Unix-socket transport equivalence, tunnel forwarding, and rate-limited page retry against a real service."""
import http.client
import http.server
import json
import os
import pathlib
import select
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading

from check_upgrade_live import (Service, V1, V2, V3, ENV, push, write, digests, assert_no_secrets)


class UnixRelay(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True


class Forward(socketserver.BaseRequestHandler):
    def handle(self):
        upstream = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        upstream.connect(str(self.server.upstream))
        with upstream:
            peers = [self.request, upstream]
            reading = list(peers)
            while reading:
                ready, _, _ = select.select(reading, [], [], 120)
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
    def address_string(self):
        return "unix"

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
        raw_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        raw_sock.settimeout(120)
        raw_sock.connect(str(self.server.upstream))
        connection = http.client.HTTPConnection("localhost", timeout=120)
        connection.sock = raw_sock
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
        base, _ = digests(binary, old, new)
        outputs = []
        for mode in ("direct", "recording", "forward", "throttled"):
            data = pathlib.Path(root) / mode
            data.mkdir()
            service = Service(binary, data, old)
            server = None
            proxy_sock = pathlib.Path(root) / f"{mode}-proxy.sock"
            try:
                target_sock = service.admin_socket
                if mode != "direct":
                    handler = Forward if mode == "forward" else Recording
                    server = UnixRelay(str(proxy_sock), handler)
                    os.chmod(proxy_sock, 0o600)
                    server.upstream, server.service = service.admin_socket, service
                    server.record, server.throttle = [], mode == "throttled"
                    server.retry_body, server.retried = None, False
                    thread = threading.Thread(target=server.serve_forever, daemon=True)
                    thread.start()
                    target_sock = proxy_sock
                # Successful subsequent open proves an earlier refusal reserved no slot.
                refused = push(binary, service, invalid, base, admin_socket=target_sock)
                assert refused.returncode == 1 and refused.stderr.startswith(b"error E1421:"), \
                    (mode, refused)
                if mode == "throttled":
                    assert server.retried and server.retry_body is None, server.record
                    server.throttle, server.retried = True, False
                accepted = push(binary, service, new, None, admin_socket=target_sock)
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
                proxy_sock.unlink(missing_ok=True)
                service.stop()
        assert all(output == outputs[0] for output in outputs), outputs
    print("remote upgrades: direct/proxy/forward reports match and rate retry passed")


def tunnel(binary):
    binary = str(pathlib.Path(binary).resolve())
    with tempfile.TemporaryDirectory() as root:
        old, new = write(root, "old", V1), write(root, "new", V2)
        query = write(root, "query", V1 + "\nfn total(state: Int) : Int\n  state\nend\n")
        data = pathlib.Path(root) / "data"
        data.mkdir()
        service = Service(binary, data, old)
        forwarded_sock = pathlib.Path(root) / "forwarded.sock"
        relay = UnixRelay(str(forwarded_sock), Forward)
        os.chmod(forwarded_sock, 0o600)
        relay.upstream = service.admin_socket
        thread = threading.Thread(target=relay.serve_forever, daemon=True)
        thread.start()
        try:
            assert service.value("add", amount="5") == "5"
            seen = []
            for sock in (service.admin_socket, forwarded_sock):
                state = pathlib.Path(root) / "state.moex"
                fetched = subprocess.run([binary, "fetch", "--admin-socket", str(sock),
                                          "--export", "state", "--out", str(state)],
                                         capture_output=True, timeout=300, env=ENV)
                assert fetched.returncode == 0, fetched
                queried = subprocess.run([binary, "query", str(query), "total", "--export",
                                          str(state)], capture_output=True, timeout=60)
                live_queried = subprocess.run([binary, "query", str(query), "total",
                                               "--admin-socket", str(sock)],
                                              capture_output=True, timeout=60, env=ENV)
                assert queried.stdout == b"5\n" and live_queried.stdout == b"5\n", (
                    queried, live_queried)
                seen.append((queried.stdout, live_queried.stdout))
                state.unlink()
            assert seen[0] == seen[1], "tunnel changed an export or live query"
            accepted = push(binary, service, new, None, admin_socket=forwarded_sock)
            assert accepted.returncode == 0, accepted
            assert service.value("read") == "v2:5"
            assert_no_secrets(accepted.stdout, accepted.stderr)
        finally:
            relay.shutdown()
            relay.server_close()
            thread.join()
            forwarded_sock.unlink(missing_ok=True)
            service.stop()
    print("tunnel: forwarded Unix socket push, live query and state export match direct")


if __name__ == "__main__":
    run(sys.argv[1])
    tunnel(sys.argv[1])
