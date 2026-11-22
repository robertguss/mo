#!/usr/bin/env python3
"""Differential cross-check of Mo Crypto and Bytes encodings against Python stdlib."""

import base64
import hashlib
import hmac
import pathlib
import random
import subprocess
import sys
import tempfile


def mo_escape_bytes_expr(b: bytes) -> str:
    return f'must_hex("{b.hex()}")'


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: ci/check_crypto.py PATH_TO_MO", file=sys.stderr)
        return 2
    mo = sys.argv[1]
    rng = random.Random(20261121)

    sample_bytes = [
        b"",
        b"\x00",
        b"\xff",
        b"abc",
        b"\x00\x01\x02\xfb\xfc\xfd\xfe\xff",
        b"The quick brown fox jumps over the lazy dog",
    ] + [rng.randbytes(n) for n in (1, 2, 3, 4, 15, 16, 31, 32, 63, 64, 65, 131, 257)]

    lines = [
        "module Main",
        "use Crypto",
        "",
        "fn must_hex(h: Text) : Bytes",
        "  case Bytes.from_hex(h)",
        '    Some(b): b',
        '    None: "".bytes',
        "  end",
        "end",
        "",
        'test "differential hex, base64url, sha256, hmac_sha256 and scrypt vs Python"',
    ]

    for i, b in enumerate(sample_bytes):
        hex_want = b.hex()
        b64_want = base64.urlsafe_b64encode(b).decode("ascii").rstrip("=")
        sha_want = hashlib.sha256(b).hexdigest()
        lines.append(f'  b_{i} = must_hex("{hex_want}")')
        lines.append(f'  assert b_{i}.to_hex == "{hex_want}"')
        lines.append(f'  assert Bytes.from_hex("{hex_want.upper()}") == Some(b_{i})')
        lines.append(f'  assert b_{i}.to_base64url == "{b64_want}"')
        lines.append(f'  assert Bytes.from_base64url("{b64_want}") == Some(b_{i})')
        lines.append(f'  assert Crypto.sha256(b_{i}).to_hex == "{sha_want}"')

    for i in range(12):
        key = rng.randbytes(rng.choice([0, 1, 16, 32, 64, 65, 131]))
        msg = rng.randbytes(rng.choice([0, 1, 17, 64, 199]))
        mac_want = hmac.new(key, msg, hashlib.sha256).hexdigest()
        lines.append(
            f'  assert Crypto.hmac_sha256(must_hex("{key.hex()}"), must_hex("{msg.hex()}")).to_hex == "{mac_want}"'
        )

    # Cross-check scrypt PHC produced by Python hashlib.scrypt -> verified by Mo Crypto.verify_password
    scrypt_cases = [
        ("", b"", 4, 1, 1, 32),
        ("hunter2", b"0123456789abcdef", 10, 8, 1, 32),
        ("p@ssw0rd-\u00e9-\U0001f512", rng.randbytes(24), 12, 8, 2, 64),
    ]
    for pw, salt, ln, r, p, dklen in scrypt_cases:
        dk = hashlib.scrypt(pw.encode("utf-8"), salt=salt, n=1 << ln, r=r, p=p, dklen=dklen)
        s64 = base64.b64encode(salt).decode("ascii").rstrip("=")
        h64 = base64.b64encode(dk).decode("ascii").rstrip("=")
        phc = f"$scrypt$ln={ln},r={r},p={p}${s64}${h64}"
        pw_esc = pw.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'  assert Crypto.verify_password("{pw_esc}", "{phc}")')
        lines.append(f'  assert not Crypto.verify_password("{pw_esc}x", "{phc}")')

    lines.append("end")
    lines.append("")

    # Also print a Mo-generated PHC hash so Python independently re-derives and checks it!
    salt_hex = rng.randbytes(16).hex()
    lines.extend([
        "fn main(console: Console)",
        f'  case Crypto.hash_password("arvo-cross-check", must_hex("{salt_hex}"))',
        "    Ok(phc): console.print(phc)",
        '    Error(_): console.print("ERR")',
        "  end",
        "end",
        "",
    ])

    with tempfile.TemporaryDirectory(prefix="mo-check-crypto-") as tmpdir:
        src = pathlib.Path(tmpdir) / "main.mo"
        src.write_text("\n".join(lines), encoding="utf-8")

        t = subprocess.run([mo, "test", str(src)], capture_output=True, text=True, timeout=60)
        if t.returncode != 0:
            print(f"mo test failed:\n{t.stdout}\n{t.stderr}", file=sys.stderr)
            return 1

        r = subprocess.run([mo, "run", str(src)], capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            print(f"mo run failed:\n{r.stdout}\n{r.stderr}", file=sys.stderr)
            return 1
        phc = r.stdout.strip()
        parts = phc.split("$")
        if len(parts) != 5 or parts[0] != "" or parts[1] != "scrypt" or parts[2] != "ln=15,r=8,p=1":
            print(f"unexpected PHC output from Mo: {phc!r}", file=sys.stderr)
            return 1
        salt_b = base64.b64decode(parts[3] + "=" * ((-len(parts[3])) % 4))
        hash_b = base64.b64decode(parts[4] + "=" * ((-len(parts[4])) % 4))
        if salt_b != bytes.fromhex(salt_hex):
            print("PHC salt mismatch", file=sys.stderr)
            return 1
        want_dk = hashlib.scrypt(
            b"arvo-cross-check", salt=salt_b, n=1 << 15, r=8, p=1, maxmem=64 * 1024 * 1024, dklen=len(hash_b)
        )
        if hash_b != want_dk:
            print("Python scrypt digest mismatch vs Mo Crypto.hash_password", file=sys.stderr)
            return 1

    print(f"check_crypto: {mo} matched Python hashlib/hmac/scrypt/base64")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
