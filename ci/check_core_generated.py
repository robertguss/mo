#!/usr/bin/env python3
"""Generated well-typed Core programs (ci/gen_core.py) must pass both Core checkers, and their
text must survive C's parser and printer and Lean's unchanged.

    check_core_generated.py TEST_CORE_CHECK_DRIVER...

Each driver is a built tests/test_core_check.test; the tests/test_core_print.test beside it is the
printer. MO_CORE_GENERATED sets the program count (default 2000) and MO_CORE_GENERATED_SEED the
first seed (default 0), so a nightly run can cover seeds the pull request run does not.
Lean's mocore decides the same programs: a typing or parse verdict from it fails the run, and
`mocore --canonical` must print every program byte for byte as it was generated. All storage is
disposable.
"""

import concurrent.futures
import os
import pathlib
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gen_core  # noqa: E402

BATCH = 128


def c_batches(driver, files):
    for start in range(0, len(files), BATCH):
        yield start, subprocess.run(
            [driver] + files[start : start + BATCH],
            capture_output=True,
            check=True,
            timeout=120,
        ).stdout


def printed(stdout):
    """Split test_core_print's "R LENGTH\\n" + bytes stream into the texts it printed."""
    texts, at = [], 0
    while at < len(stdout):
        end = stdout.index(b"\n", at)
        kind, _, number = stdout[at:end].partition(b" ")
        at = end + 1
        if kind == b"E":
            texts.append("E" + number.decode())
            continue
        assert kind == b"R", stdout[at - 20 : at + 20]
        texts.append(stdout[at : at + int(number)].decode("utf-8"))
        at += int(number)
    return texts


def lean(oracle, args):
    result = subprocess.run([oracle] + args, capture_output=True, timeout=60)
    return result.returncode, result.stdout.decode("utf-8", "replace")


def main():
    started = time.monotonic()
    drivers = sys.argv[1:]
    if not drivers:
        raise AssertionError("at least one test_core_check driver is required")
    count = int(os.environ.get("MO_CORE_GENERATED", "2000"))
    first = int(os.environ.get("MO_CORE_GENERATED_SEED", "0"))
    oracle = pathlib.Path(os.environ.get("MO_MOCORE", "formal/.lake/build/bin/mocore")).resolve()
    if not oracle.exists():
        raise AssertionError(f"{oracle} is not built; run `make formal` first")
    texts = [gen_core.document(first + i) for i in range(count)]
    with tempfile.TemporaryDirectory(prefix="mo-core-generated-") as tmp:
        files = []
        for i, text in enumerate(texts):
            path = pathlib.Path(tmp) / f"{first + i}.core"
            path.write_text(text, encoding="utf-8")
            files.append(str(path))
        for driver in drivers:
            printer = str(pathlib.Path(driver).with_name("test_core_print.test"))
            verdicts = []
            for _, out in c_batches(driver, files):
                verdicts += out.decode().splitlines()
            bad = [(first + i, v) for i, v in enumerate(verdicts) if v != "0"]
            if len(verdicts) != count or bad:
                seed, verdict = bad[0] if bad else (first, "missing verdicts")
                raise AssertionError(
                    f"{driver}: C refused generated program, seed {seed}: {verdict}\n{texts[seed - first]}"
                )
            output = []
            for _, out in c_batches(printer, files):
                output += printed(out)
            for i, (want, got) in enumerate(zip(texts, output)):
                if want != got:
                    raise AssertionError(
                        f"{printer}: seed {first + i} did not round trip\nwanted {want}\ngot    {got}"
                    )
            if len(output) != count:
                raise AssertionError(f"{printer}: printed {len(output)} of {count} programs")

        def lean_case(item):
            i, path = item
            code, text = lean(oracle, [path])
            if code in (4, 5):
                raise AssertionError(
                    f"mocore refused generated program, seed {first + i} (exit {code}): {text}\n{texts[i]}"
                )
            code, text = lean(oracle, ["--canonical", path])
            if code != 0 or text != texts[i]:
                raise AssertionError(
                    f"mocore printed seed {first + i} differently (exit {code})\nwanted {texts[i]}\ngot    {text}"
                )

        with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 2)) as pool:
            list(pool.map(lean_case, enumerate(files)))
    print(
        f"Generated Core: {count} programs (seeds {first}..{first + count - 1}) pass both checkers "
        f"and print identically in C ({len(drivers)} builds) and Lean; {time.monotonic() - started:.2f}s"
    )


if __name__ == "__main__":
    main()
