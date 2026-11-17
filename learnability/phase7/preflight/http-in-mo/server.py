import json
import socketserver
import sys

CHUNK = 8192


def body(size: int) -> bytes:
    items = []
    total = 2
    n = 0
    while total < size:
        n += 1
        item = {
            "id": n,
            "name": f"Customer {n}",
            "email": f"customer{n}@example.com",
            "active": n % 3 != 0,
            "balance": round(n * 12.37, 2),
            "tags": ["new", "café", f"tier-{n % 5}"],
            "note": None,
            "address": {"city": "Zürich", "postcode": f"{8000 + n % 100}", "country": "CH"},
            "bio": "Likes long walks, short meetings and well-formed JSON. ☕ " * 2,
        }
        items.append(item)
        total += len(json.dumps(item, ensure_ascii=False, separators=(",", ":")).encode()) + 1
    return json.dumps(items, ensure_ascii=False, separators=(",", ":")).encode()


class Handler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        request = b""
        while b"\r\n\r\n" not in request:
            more = self.request.recv(4096)
            if not more:
                return
            request += more
        _, path, _ = request.split(b"\r\n", 1)[0].split(b" ")
        _, kind, size = path.decode().split("/")
        payload = body(int(size))
        head = b"HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nConnection: close\r\n"
        if kind == "fixed":
            self.wfile.write(head + f"Content-Length: {len(payload)}\r\n\r\n".encode() + payload)
            return
        self.wfile.write(head + b"Transfer-Encoding: chunked\r\n\r\n")
        for at in range(0, len(payload), CHUNK):
            piece = payload[at : at + CHUNK]
            self.wfile.write(f"{len(piece):x}\r\n".encode() + piece + b"\r\n")
        self.wfile.write(b"0\r\n\r\n")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    if sys.argv[1] == "--body":
        sys.stdout.buffer.write(body(int(sys.argv[2])))
    else:
        Server(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
