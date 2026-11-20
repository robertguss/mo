#!/usr/bin/env python3
"""The Http client and the real network (plain TCP and TLS) against live peers on loopback.

Run from the repository root: python3 ci/check_network.py MO

One Mo program sends one request with `mo run` and prints the status, a header and the body it
got back, or returns the error from `main`. The peers are raw and TLS loopback sockets that record
what arrived and answer by path. The program's timeout is TIMEOUT seconds and the driver waits
WAIT seconds for it, so the driver never gives up before the program does. Each refusal is
followed by the same request to the same peer succeeding.
"""

import os
import pathlib
import socket
import ssl
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


def serve_http(connection, requests, clean_tls_close=False):
    connection.settimeout(WAIT)
    request = b""
    while b"\r\n\r\n" not in request:
        chunk = connection.recv(4096)
        if not chunk:
            return
        request += chunk
    head, _, body = request.partition(b"\r\n\r\n")
    length = next(
        int(line.split(b":")[1])
        for line in head.split(b"\r\n")
        if line.lower().startswith(b"content-length:")
    )
    while len(body) < length:
        chunk = connection.recv(4096)
        if not chunk:
            return
        body += chunk
    requests.append(head + b"\r\n\r\n" + body)
    path = head.split(b" ")[1].split(b"?")[0]
    pieces = REPLIES[path]
    if pieces is None:
        connection.recv(1)
        return
    for piece in pieces:
        connection.sendall(piece)
        time.sleep(0.05)
    if clean_tls_close:
        try:
            connection.unwrap()
        except OSError:
            pass


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
            serve_http(connection, self.requests)


class TlsPeer:
    def __init__(self, cert_path, key_path):
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.minimum_version = ssl.TLSVersion.TLSv1_2
        self.context.load_cert_chain(certfile=str(cert_path), keyfile=str(key_path))
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.port = self.listener.getsockname()[1]
        self.requests = []
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            raw, _ = self.listener.accept()
            threading.Thread(target=self.answer, args=(raw,), daemon=True).start()

    def answer(self, raw):
        with raw:
            raw.settimeout(WAIT)
            try:
                tls = self.context.wrap_socket(raw, server_side=True)
            except (ssl.SSLError, OSError):
                return
            with tls:
                serve_http(tls, self.requests, clean_tls_close=True)


class RawReplyPeer:
    def __init__(self, reply=b""):
        self.reply = reply
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.port = self.listener.getsockname()[1]
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            raw, _ = self.listener.accept()
            with raw:
                try:
                    raw.recv(512)
                    if self.reply:
                        raw.sendall(self.reply)
                except OSError:
                    pass


def make_certificates(folder):
    ca_dir = folder / "ca"
    ca_dir.mkdir()
    (ca_dir / "certs").mkdir()
    (ca_dir / "index.txt").write_text("")
    (ca_dir / "serial").write_text("1000\n")
    cnf = ca_dir / "openssl.cnf"
    cnf.write_text(
        f"""[ ca ]
default_ca = mo_ca

[ mo_ca ]
dir = {ca_dir}
new_certs_dir = $dir/certs
database = $dir/index.txt
serial = $dir/serial
private_key = $dir/ca.key
certificate = $dir/ca.pem
default_md = sha256
policy = policy_any
unique_subject = no
copy_extensions = copy

[ policy_any ]
commonName = optional

[ req ]
distinguished_name = req_dn
prompt = no

[ req_dn ]
CN = Mo Test

[ ca_ext ]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign

[ valid_ext ]
basicConstraints = CA:FALSE
subjectAltName = IP:127.0.0.1,DNS:localhost

[ wrong_ext ]
basicConstraints = CA:FALSE
subjectAltName = DNS:other.example
"""
    )

    def openssl(*args):
        subprocess.run(["openssl", *args], check=True, capture_output=True)

    ca_key = ca_dir / "ca.key"
    ca_pem = ca_dir / "ca.pem"
    openssl(
        "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", str(ca_key), "-out", str(ca_pem),
        "-days", "3650", "-config", str(cnf), "-extensions", "ca_ext",
    )

    def sign_leaf(name, ext_section, startdate=None, enddate=None):
        key = folder / f"{name}.key"
        csr = folder / f"{name}.csr"
        pem = folder / f"{name}.pem"
        openssl(
            "req", "-newkey", "rsa:2048", "-nodes",
            "-keyout", str(key), "-out", str(csr),
            "-subj", f"/CN={name}", "-addext",
            "subjectAltName = IP:127.0.0.1,DNS:localhost"
            if ext_section == "valid_ext"
            else "subjectAltName = DNS:other.example",
        )
        ca_args = [
            "ca", "-batch", "-notext", "-config", str(cnf),
            "-extensions", ext_section, "-in", str(csr), "-out", str(pem),
        ]
        if startdate and enddate:
            ca_args.extend(["-startdate", startdate, "-enddate", enddate])
        else:
            ca_args.extend(["-days", "3650"])
        openssl(*ca_args)
        return pem, key

    valid_pem, valid_key = sign_leaf("valid", "valid_ext")
    wrong_pem, wrong_key = sign_leaf("wrong", "wrong_ext")
    expired_pem, expired_key = sign_leaf(
        "expired", "valid_ext", startdate="200101000000Z", enddate="200102000000Z"
    )
    untrusted_key = folder / "untrusted.key"
    untrusted_pem = folder / "untrusted.pem"
    openssl(
        "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", str(untrusted_key), "-out", str(untrusted_pem),
        "-days", "3650", "-subj", "/CN=untrusted",
        "-addext", "subjectAltName = IP:127.0.0.1,DNS:localhost",
    )
    return {
        "ca": ca_pem,
        "valid": (valid_pem, valid_key),
        "wrong": (wrong_pem, wrong_key),
        "expired": (expired_pem, expired_key),
        "untrusted": (untrusted_pem, untrusted_key),
    }


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    peer = Peer()
    here = f"http://127.0.0.1:{peer.port}"
    with tempfile.TemporaryDirectory() as raw_folder:
        folder = pathlib.Path(raw_folder)
        program = folder / "main.mo"
        program.write_text(PROGRAM)
        certs = make_certificates(folder)
        tls_env = {**os.environ, "SSL_CERT_FILE": str(certs["ca"])}

        valid_tls = TlsPeer(*certs["valid"])
        wrong_tls = TlsPeer(*certs["wrong"])
        expired_tls = TlsPeer(*certs["expired"])
        untrusted_tls = TlsPeer(*certs["untrusted"])
        drop_peer = RawReplyPeer()
        not_tls_peer = RawReplyPeer(b"HTTP/1.1 400 Bad Request\r\n\r\n")
        secure_here = f"https://127.0.0.1:{valid_tls.port}"

        def run(url, env=None):
            start = time.monotonic()
            done = subprocess.run(
                [str(binary), "run", str(program), url],
                cwd=root,
                env=env,
                capture_output=True,
                timeout=WAIT,
            )
            return (done.returncode, done.stdout, done.stderr), time.monotonic() - start

        def succeeds(path, reply_header, target_peer=peer, base=here, host_header=None, env=None):
            before = len(target_peer.requests)
            result, elapsed = run(base + path, env=env)
            expected = b"200\n" + reply_header + b"\n" + BODY + b"\n"
            assert result == (0, expected, b""), (base + path, result)
            authority = (host_header or f"127.0.0.1:{target_peer.port}").encode()
            assert target_peer.requests[before:] == [
                b"PUT " + path.encode() + b"?q=a%20b HTTP/1.1\r\n"
                b"Host: " + authority + b"\r\n"
                b"Connection: close\r\n"
                b"Content-Length: 6\r\n"
                b"Accept: text/plain\r\n"
                b"Content-Type: text/plain; charset=utf-8\r\n"
                b"X-Two: 2\r\n"
                b"\r\n" + "héllo".encode()
            ], (base + path, target_peer.requests[before:])
            return elapsed

        def refused(url, error, env=None):
            result, elapsed = run(url, env=env)
            assert result == (1, b"", error.encode() + b"\n"), (url, result)
            usual = succeeds("/ok", b"yes")
            return elapsed, usual

        def refused_prefix(url, prefix, env=None):
            result, _ = run(url, env=env)
            code, out, err = result
            text = err.decode("utf-8", errors="replace")
            assert code == 1 and out == b"" and text.startswith(prefix) and text.endswith("))\n"), (
                url,
                result,
            )
            succeeds("/ok", b"yes", target_peer=valid_tls, base=secure_here, env=tls_env)

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

        # Live HTTPS checks: valid IP SAN, valid DNS SAN + SNI, chunked, close-notify, and failures.
        succeeds("/ok", b"yes", target_peer=valid_tls, base=secure_here, env=tls_env)
        succeeds(
            "/ok",
            b"yes",
            target_peer=valid_tls,
            base=f"https://localhost:{valid_tls.port}",
            host_header=f"localhost:{valid_tls.port}",
            env=tls_env,
        )
        succeeds("/chunked", b"chunked", target_peer=valid_tls, base=secure_here, env=tls_env)
        succeeds("/close", b"close", target_peer=valid_tls, base=secure_here, env=tls_env)

        refused_prefix(secure_here + "/ok", 'Unreachable(Certificate(reason: "')
        refused_prefix(
            f"https://127.0.0.1:{expired_tls.port}/ok",
            'Unreachable(Certificate(reason: "certificate has expired',
            env=tls_env,
        )
        refused_prefix(
            f"https://127.0.0.1:{wrong_tls.port}/ok",
            'Unreachable(Certificate(reason: "IP address mismatch',
            env=tls_env,
        )
        refused_prefix(
            f"https://localhost:{wrong_tls.port}/ok",
            'Unreachable(Certificate(reason: "hostname mismatch',
            env=tls_env,
        )
        refused_prefix(
            f"https://127.0.0.1:{untrusted_tls.port}/ok",
            'Unreachable(Certificate(reason: "self',
            env=tls_env,
        )
        refused(
            f"https://127.0.0.1:{drop_peer.port}/ok",
            "Unreachable(Closed)",
            env=tls_env,
        )
        refused_prefix(
            f"https://127.0.0.1:{not_tls_peer.port}/ok",
            'Unreachable(Broken(reason: "',
            env=tls_env,
        )

        narrowed_program = folder / "narrowed.mo"
        narrowed_program.write_text(
            f"""module Narrowed
use Http

fn main(args: Args, console: Console, network: Network) : Result(Unit, Http.HttpError)
  url = args.list.first or ""
  allowed = network.allow_hosts(["127.0.0.1:{peer.port}", "127.0.0.1:{valid_tls.port}"])
  response = try Http.request("PUT", url).text("ok").within({TIMEOUT}.seconds).send(allowed)
  console.print("#{{response.status}}")
  Ok(Unit)
end
"""
        )
        ok_run = subprocess.run(
            [str(binary), "run", str(narrowed_program), secure_here + "/ok"],
            cwd=root,
            env=tls_env,
            capture_output=True,
            timeout=WAIT,
        )
        assert ok_run.returncode == 0 and ok_run.stdout == b"200\n", ok_run
        blocked = Peer()
        bad_run = subprocess.run(
            [str(binary), "run", str(narrowed_program), f"https://127.0.0.1:{blocked.port}/ok"],
            cwd=root,
            env=tls_env,
            capture_output=True,
            timeout=WAIT,
        )
        assert (
            bad_run.returncode == 1
            and bad_run.stderr
            == f'Unreachable(Refused(host: "127.0.0.1", port: {blocked.port}))\n'.encode()
            and blocked.requests == []
        ), (bad_run, blocked.requests)

    print("network: 19 checks against live HTTP and HTTPS peers pass, each refusal beside a success")


if __name__ == "__main__":
    main()
