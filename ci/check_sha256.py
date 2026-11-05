#!/usr/bin/env python3
"""Compare the native digest with hashlib, including its running executable."""
import hashlib
import pathlib
import random
import subprocess
import sys


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    expected = hashlib.sha256(executable.read_bytes()).hexdigest()
    output = subprocess.check_output([str(executable), "identity"], text=True).splitlines()
    actual = output[-1]
    assert actual == expected, (actual, expected)
    configuration = executable.parent.parent.name
    assert configuration in ("debug", "sanitize"), configuration
    assert output[-2].endswith("; " + configuration), output[-2]
    rng = random.Random(164)
    for length in [0, 1, 55, 56, 63, 64, 65, 127, 128, 1000000] + [
        rng.randrange(65536) for _ in range(100)
    ]:
        data = rng.randbytes(length)
        actual = subprocess.check_output([str(executable), "stdin"], input=data).strip()
        expected = hashlib.sha256(data).hexdigest().encode()
        assert actual == expected, length
    print("sha256: executable identity and 110 reference comparisons passed")


if __name__ == "__main__":
    main()
