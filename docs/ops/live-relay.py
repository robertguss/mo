#!/usr/bin/env python3
"""Serve plain HTTP on a loopback port and forward each POST to a service's HTTPS route.

usage: live-relay.py PORT URL    (for example: live-relay.py 8000 https://HOST)
"""
import http.client
import http.server
import sys
import urllib.parse

DROPPED = ("host", "content-length", "connection")


class Relay(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        headers = {k: v for k, v in self.headers.items() if k.lower() not in DROPPED}
        target = self.server.target
        kind = http.client.HTTPSConnection if target.scheme == "https" else http.client.HTTPConnection
        upstream = kind(target.netloc, timeout=300)
        try:
            upstream.request("POST", self.path, body, headers)
            response = upstream.getresponse()
            data = response.read()
        except OSError:
            self.send_error(502)
            return
        finally:
            upstream.close()
        self.send_response(response.status)
        for name in ("Content-Type", "MCP-Session-Id", "Retry-After"):
            if response.getheader(name):
                self.send_header(name, response.getheader(name))
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)


def main(port, url):
    server = http.server.ThreadingHTTPServer(("127.0.0.1", int(port)), Relay)
    server.target = urllib.parse.urlsplit(url)
    print(f"relay: listening on http://127.0.0.1:{server.server_address[1]}/mcp", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip())
    main(*sys.argv[1:])
