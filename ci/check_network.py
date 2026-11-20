#!/usr/bin/env python3
"""The Http client and the real network against a peer on loopback.

Run from the repository root: python3 ci/check_network.py MO

One Mo program sends one request with `mo run` and prints the status, a header and the body it
got back, or returns the error from `main`. The peer is a raw socket that records what arrived
and answers by path. The program's timeout is TIMEOUT seconds and the driver waits WAIT seconds
for it, so the driver never gives up before the program does. Each refusal is followed by the
same request to the same peer succeeding.
"""

import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time

root = pathlib.Path(__file__).resolve().parent.parent
TIMEOUT = 5
WAIT = 120

PROGRAM = f"""module Main
use Http

fn main(args: Args, console: Console, network: Network) : Result(Unit, Http.HttpError)
  url = args.list.first or ""
  asked = Http.request("PUT", url).query("q", "a b").header("X-Two", "2")
    .header("Accept", "text/plain").text("héllo").within({TIMEOUT}.seconds)
  response = try asked.send(network)
  console.print("#{{response.status}}")
  console.print(response.header("x-reply") or "none")
  console.print(response.text or "not text")
  Ok(Unit)
end
"""

BODY = b"hello from the peer"
REPLIES = {
    b"/ok": [b"HTTP/1.1 200 OK\r\nX-Reply: yes\r\nContent-Length: 19\r\n\r\n" + BODY],
    b"/chunked": [
        b"HTTP/1.1 200 OK\r\nX-Reply: chunked\r\nTransfer-Encoding: chunked\r\n\r\n6\r\nhel",
        b"lo \r\nd;note=1\r\nfrom the peer\r\n",
        b"0\r\nX-Trailer: 1\r\n\r\n",
    ],
    b"/close": [b"HTTP/1.1 200 OK\r\nX-Reply: close\r\n\r\n" + BODY[:9], BODY[9:]],
    b"/cut": [b"HTTP/1.1 200 OK\r\nX-Reply: yes\r\nContent-Length: 19\r\n\r\n" + BODY[:9]],
    b"/silent": None,
}


class Peer:
    def __init__(self):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.port = self.listener.getsockname()[1]
        self.requests = []
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            connection, _ = self.listener.accept()
            threading.Thread(target=self.answer, args=(connection,), daemon=True).start()

    def answer(self, connection):
        with connection:
            connection.settimeout(WAIT)
            request = b""
            while b"\r\n\r\n" not in request:
                request += connection.recv(4096)
            head, _, body = request.partition(b"\r\n\r\n")
            length = next(
                int(line.split(b":")[1])
                for line in head.split(b"\r\n")
                if line.lower().startswith(b"content-length:")
            )
            while len(body) < length:
                body += connection.recv(4096)
            self.requests.append(head + b"\r\n\r\n" + body)
            pieces = REPLIES[head.split(b" ")[1].split(b"?")[0]]
            if pieces is None:
                connection.recv(1)
                return
            for piece in pieces:
                connection.sendall(piece)
                time.sleep(0.05)


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    peer = Peer()
    here = f"http://127.0.0.1:{peer.port}"
    with tempfile.TemporaryDirectory() as folder:
        program = pathlib.Path(folder) / "main.mo"
        program.write_text(PROGRAM)

        def run(url):
            start = time.monotonic()
            done = subprocess.run(
                [str(binary), "run", str(program), url], cwd=root, capture_output=True, timeout=WAIT
            )
            return (done.returncode, done.stdout, done.stderr), time.monotonic() - start

        def succeeds(path, reply_header):
            before = len(peer.requests)
            result, elapsed = run(here + path)
            expected = b"200\n" + reply_header + b"\n" + BODY + b"\n"
            assert result == (0, expected, b""), (path, result)
            assert peer.requests[before:] == [
                b"PUT " + path.encode() + b"?q=a%20b HTTP/1.1\r\n"
                b"Host: 127.0.0.1:" + str(peer.port).encode() + b"\r\n"
                b"Connection: close\r\n"
                b"Content-Length: 6\r\n"
                b"Accept: text/plain\r\n"
                b"Content-Type: text/plain; charset=utf-8\r\n"
                b"X-Two: 2\r\n"
                b"\r\n" + "héllo".encode()
            ], (path, peer.requests[before:])
            return elapsed

        def refused(url, error):
            result, elapsed = run(url)
            assert result == (1, b"", error.encode() + b"\n"), (url, result)
            usual = succeeds("/ok", b"yes")
            return elapsed, usual

        succeeds("/ok", b"yes")
        succeeds("/chunked", b"chunked")
        succeeds("/close", b"close")

        elapsed, usual = refused(here + "/silent", "Unreachable(TimedOut)")
        assert TIMEOUT <= elapsed <= TIMEOUT + usual + 10, (elapsed, usual)

        refused(
            here + "/cut",
            'BadResponse(reason: "the connection closed before the response was whole")',
        )

        closed = socket.socket()
        closed.bind(("127.0.0.1", 0))
        port = closed.getsockname()[1]
        closed.close()
        refused(
            f"http://127.0.0.1:{port}/ok",
            f'Unreachable(Refused(host: "127.0.0.1", port: {port}))',
        )

        name = "a" * 254
        refused(f"http://{name}/ok", f'Unreachable(UnknownHost(host: "{name}"))')

    print("network: 7 checks against a live peer pass, each refusal beside a success")


if __name__ == "__main__":
    main()
