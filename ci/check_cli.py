#!/usr/bin/env python3
"""Black-box CLI contracts and goldens, with Python's real JSON parser."""
import errno
import json
import os
import pathlib
import resource
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent


def run(binary, args, status):
    result = subprocess.run([str(binary), *args], cwd=root, capture_output=True, timeout=30)
    assert result.returncode == status, (args, result.returncode, result.stderr)
    return result


def records(result, command):
    assert not result.stderr, result.stderr
    lines = result.stdout.decode("utf-8").splitlines()
    assert result.stdout.endswith(b"\n")
    items = [json.loads(line) for line in lines]
    assert items[0] == {"record": "version", "format": "1.0", "mo": "0.0.0", "command": command}
    assert items[-1]["record"] == "totals"
    assert items[-1]["status"] == result.returncode
    assert items[-1]["errors"] == len(items) - 2
    return items


def source_stack(binary, folder):
    # This fixture exercises the entire precedence ladder at the source-depth
    # boundary, not only nested brackets. The process stack stays small while
    # loading, parsing and inference must use the explicit source-work stack.
    path = folder / "main.mo"
    header = ("module Main\nfn run(a: Bool, b: Bool, c: Bool, d: Range, e: Int, "
              "f: Int, g: Int, h: fn(Bool) : Int) : Bool\n")

    def small_stack():
        _, hard = resource.getrlimit(resource.RLIMIT_STACK)
        resource.setrlimit(resource.RLIMIT_STACK, (4 << 20, hard))

    for depth, status in ((255, 0), (256, 1)):
        path.write_text(header + "a implies b or c and not d == e .. f + g * -h(" * depth
                        + "a" + ")" * depth + "\nend\n")
        for json_mode in (False, True):
            result = subprocess.run(
                [str(binary), "check", *(["--json"] if json_mode else []), str(path)],
                cwd=root, capture_output=True, timeout=30, preexec_fn=small_stack)
            assert result.returncode == status, (depth, json_mode, result.returncode, result.stderr)
            if json_mode:
                items = records(result, "check")
                assert items[-1]["errors"] == status and items[-1]["dropped"] == 0
                if status:
                    assert items[1]["code"] == "E1006"
            else:
                assert not result.stdout
                assert (b"E1006" in result.stderr) if status else not result.stderr


flags = {
    "--json": ([], {"check", "test"}),
    "--list": ([], {"test"}),
    "--reference": ([], {"guide"}),
    "--only": (["Main.test.1"], {"test"}),
    "--in": (["Main"], {"test"}),
    "--max-stack": (["1MiB"], {"run", "test"}),
    "--max-depth": (["100"], {"run", "test"}),
    "--max-heap": (["1GiB"], {"run", "test"}),
    "--calls": (["30"], {"test"}),
    "--seconds": (["3"], {"test"}),
    "--emit=core": ([], {"check"}),
    "--emit=bytecode": ([], {"check"}),
}


def check(binary):
    driver = binary.parent / "tests/test_cli.test"
    for stage in ("parse", "load", "modules", "signatures", "bodies", "fix", "multiline"):
        path = f"tests/cli/{stage}/main.mo"
        for json_mode in (False, True):
            result = run(binary, ["check", *(["--json"] if json_mode else []), path], 1)
            actual = result.stdout if json_mode else result.stderr
            expected = root / f"tests/cli/{stage}/{'json' if json_mode else 'text'}.golden"
            assert actual == expected.read_bytes(), (binary, expected, actual)
            if json_mode:
                items = records(result, "check")
                if stage == "bodies":
                    assert [d["primary"]["line"] for d in items[1:-1]] == [3, 6]
                if stage == "fix":
                    edit = items[1]["fixes"][0]["edits"][0]
                    # 12 bytes for the header + 7 for "fn run(".
                    assert edit["text"] == "_" and edit["span"]["start"] == edit["span"]["end"] == 19
            else:
                assert not result.stdout
    good = "tests/cli/good/main.mo"
    result = run(binary, ["check", good], 0)
    assert not result.stdout and not result.stderr
    records(run(binary, ["check", "--json", good], 0), "check")
    for mode in ("text", "json"):
        result = run(driver, [f"--render-{mode}"], 0)
        assert not result.stderr
        assert result.stdout == (root / f"tests/cli/render-{mode}.golden").read_bytes()
        if mode == "json":
            d = json.loads(result.stdout)
            assert d["primary"]["column"] == 2
            assert d["primary"]["file"] == 'badÿ"\\.mo'
            assert d["secondary"][0]["line"] == 7
            assert d["secondary"][1]["file"] is None
    escaped = json.loads(run(driver, ["--render-bytes"], 0).stdout)
    assert escaped["headline"] == "".join(chr(i) for i in range(1, 256))
    assert escaped["primary"]["label"] == "é界😀"
    assert run(binary, ["guide", "--reference"], 0).stdout == (root / "docs/reference/README.md").read_bytes()
    guide = root / "docs/agent-guide.md"
    guide_bytes = guide.read_bytes() if guide.exists() else b""
    if guide_bytes:
        assert run(binary, ["guide"], 0).stdout == guide_bytes
    else:
        result = run(binary, ["guide"], 1)
        assert not result.stdout and b"--reference" in result.stderr
    for command in ("run", "test", "format", "fix", "check"):
        args = [command, *(["--emit=core"] if command == "check" else []), good]
        result = run(binary, args, 1)
        assert not result.stdout and b"this build cannot" in result.stderr
        assert result.stderr.count(b"<unavailable>") == 1
    items = records(run(binary, ["test", "--json", "--list", "--only", "No.match", good], 1), "test")
    assert items[1]["code"] == "E1400" and items[1]["reason"] is None
    assert all(items[-1][k] == 0 for k in ("passed", "failed", "stopped"))

    def mistake(args, message=None):
        r = run(binary, args, 2)
        assert not r.stdout and r.stderr.count(b"\n") == 1, (args, r)
        if message:
            assert r.stderr == message.encode() + b"\n", (args, r.stderr)

    for args in ([], ["unknown"], [""], ["--version", "extra"], ["check"], ["format"],
                 ["fix"], ["run"], ["test"], ["guide", good], ["check", ""],
                 ["check", good, good], ["test", good, good], ["check", "--bad", good],
                 ["check", "--emit=bad", good], ["check", "--emit", "core", good],
                 ["check", "--json", "--emit=core", good], ["check", "--version", good],
                 ["check", "--json", "--bad", good]):
        mistake(args)
    mistake(["bad\n\t\xff"], 'unknown command (expected check, run, test, format, fix or guide): "bad\\x0a\\x09\\xc3\\xbf"')
    for flag, (value, allowed) in flags.items():
        for command in ("check", "run", "test", "format", "fix", "guide"):
            args = [command, flag, *value, *([] if command == "guide" else [good])]
            if command not in allowed:
                mistake(args, f'flag is not accepted by this command: "{flag}"')
            else:
                run(binary, args, 0 if flag == "--reference" or (command == "check" and flag == "--json") else 1)
                if flag != "--only":
                    mistake([command, flag, *value, flag, *value, *([] if command == "guide" else [good])])
        if value:
            for missing in ([], [""], ["--json"]):
                suffix = ''.join(f' "{v}"' for v in missing)
                mistake(["test", flag, *missing], f'flag needs a nonempty value: "{flag}"{suffix}')
    for flag in ("--max-stack", "--max-depth", "--max-heap", "--calls", "--seconds"):
        for value in ("0", "-1", "+1", " 1", "1 ", "1.5", "1KB", "1mib", "18446744073709551616"):
            mistake(["test", flag, value, good])
    mistake(["test", "--max-heap", "-1", good],
            'invalid limit (expected a positive decimal integer; bytes may use B, KiB, MiB or GiB): "--max-heap" "-1"')
    for flag in ("--max-depth", "--calls", "--seconds"):
        mistake(["test", flag, "4294967296", good])
        mistake(["test", flag, "1B", good])
    for flag in ("--max-stack", "--max-heap"):
        mistake(["test", flag, "18014398509481984KiB", good])
        for value in ("1", "1B", "1KiB", "1MiB", "1GiB", "18446744073709551615"):
            run(binary, ["test", flag, value, good], 1)
    run(binary, ["run", good, "--bad", ""], 1)
    run(binary, ["test", "--only", "a", "--only", "b", "--in", "Main", good], 1)
    run(binary, ["format", good, good], 1)
    run(binary, ["fix", good, good], 1)
    for version in ("--version", "-V"):
        assert run(binary, [version], 0).stdout == b"mo 0.0.0\n"
    read_fd, write_fd = os.pipe()
    os.close(read_fd)
    try:
        result = subprocess.run([str(binary), "guide", "--reference"], stdout=write_fd,
                                stderr=subprocess.PIPE, timeout=30)
        assert result.returncode == 0 and not result.stderr
    finally:
        os.close(write_fd)
    run(binary, ["run", good, "x" * 5000], 1)
    run(binary, ["run", good, *(str(i) for i in range(300))], 1)
    run(binary, ["run", good, *("x" * 4096 for _ in range(16))], 1)
    for command in ("format", "fix"):
        run(binary, [command, *([good] * 4096)], 1)
    run(binary, ["test", *(["--only", "x"] * 128), good], 1)
    mistake(["test", *(["--only", "x"] * 129), good])
    mistake(["test", "--only", "x" * 4097, good])
    run(binary, ["test", *(["--only", "x" * 4096] * 15), good], 1)
    mistake(["test", *(["--only", "x" * 4096] * 16), good])
    # Unavailable locations, invalid source bytes, invalid POSIX path bytes,
    # and the -- delimiter are exercised against real files, not mocks.
    records(run(binary, ["check", "--json", "tests/cli/missing.mo"], 1), "check")
    with tempfile.TemporaryDirectory() as temp:
        folder = pathlib.Path(temp)
        source_stack(binary, folder)
        path = folder / "main.mo"
        path.write_bytes(b"module Main\n\xff\n")
        d = records(run(binary, ["check", "--json", str(path)], 1), "check")[1]
        assert d["code"] == "E0001" and d["primary"]["line"] == 2
        invalid = folder / "bad\udcff"
        try:
            invalid.mkdir()
        except OSError as error:
            # APFS/HFS+ refuse non-UTF-8 names; don't hide unrelated I/O errors.
            if error.errno != errno.EILSEQ:
                raise
            print(f"{binary}: filesystem refuses non-UTF-8 names; path case skipped")
        else:
            path = invalid / "main.mo"
            path.write_text("module Main\nfn run()\nunknown\nend\n")
            d = records(run(binary, ["check", "--json", str(path)], 1), "check")[1]
            assert "badÿ" in d["primary"]["file"]
        path = folder / "main.mo"
        path.write_text("module Main\n" + "".join(f"fn f{i}()\nunknown\nend\n" for i in range(1500)))
        items = records(run(binary, ["check", "--json", str(path)], 1), "check")
        assert items[-1]["errors"] == 1000 and items[-1]["dropped"] == 500
        run(binary, ["format", "--", "-file.mo"], 1)
    print(f"{binary}: CLI goldens, argument matrix and JSON validity passed")


for executable in sys.argv[1:]:
    check(pathlib.Path(executable).resolve())
