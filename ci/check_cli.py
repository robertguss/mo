#!/usr/bin/env python3
"""Black-box CLI contracts and goldens, with Python's real JSON parser."""
import errno
import json
import locale
import os
import pathlib
import resource
import stat
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent

# Select an installed UTF-8 locale for the raw-text regression, independently
# of the CI shell's locale. C/POSIX/unavailable locales are tested separately.
for utf8_locale in ("C.UTF-8", "en_US.UTF-8", "UTF-8"):
    try:
        locale.setlocale(locale.LC_CTYPE, utf8_locale)
        break
    except locale.Error:
        pass
else:
    raise AssertionError("CLI display tests need an installed UTF-8 locale")


def run(binary, args, status, env=None):
    result = subprocess.run([str(binary), *args], cwd=root, env=env, capture_output=True, timeout=30)
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


def small_stack():
    _, hard = resource.getrlimit(resource.RLIMIT_STACK)
    resource.setrlimit(resource.RLIMIT_STACK, (1 << 20, hard))


def runner(binary):
    with tempfile.TemporaryDirectory(prefix="mo-runner-") as directory:
        path = pathlib.Path(directory) / "main.mo"
        path.write_text('module Main\nfn main(console: Console) : Int\n'
                        'console.print("hello 日本語")\n37\nend\n')
        checked = run(binary, ["check", str(path)], 0)
        assert not checked.stdout and not checked.stderr
        result = run(binary, ["run", str(path)], 37)
        assert result.stdout == "hello 日本語\n".encode() and not result.stderr
        # Emission writes indexed text, not an opaque binary or an execution
        # result. Both forms preserve the original filename and entry metadata.
        core = run(binary, ["check", "--emit=core", str(path)], 0)
        bytecode = run(binary, ["check", "--emit=bytecode", str(path)], 0)
        assert core.stdout.startswith(b"(core ") and not core.stderr
        assert b"Main.main.entry" in core.stdout and str(path).encode() in core.stdout
        assert b"Main.main.entry" in bytecode.stdout and not bytecode.stderr
        for name, result in (("core", core), ("bytecode", bytecode)):
            emitted = pathlib.Path(directory) / f"program.{name}"
            emitted.write_bytes(result.stdout)
            decoded = run(binary.parent / "tests/test_cli.test",
                          [f"--roundtrip-{name}", str(emitted)], 0)
            assert not decoded.stdout and not decoded.stderr

        for body, status, reason in (
            ("fn main()\nend", 0, None),
            ("fn main() : Int\n0\nend", 0, None),
            ("fn main() : Int\n255\nend", 255, None),
            ("fn main() : Int\n-1\nend", 70, b"exit status outside 0 to 255"),
            ("fn main() : Int\n256\nend", 70, b"exit status outside 0 to 255"),
            ("fn main() : Int\n9223372036854775807 + 1\nend", 70, b"overflow: Int"),
            ('fn main()\nstop "chosen stop"\nend', 70, b"chosen stop"),
        ):
            path.write_text("module Main\n" + body + "\n")
            result = run(binary, ["run", str(path)], status)
            assert not result.stdout
            if reason is None:
                assert not result.stderr
            else:
                assert result.stderr.startswith(b"bug: ") and reason in result.stderr
                assert str(path).encode() in result.stderr and b"Main.main" in result.stderr

        # Tail calls replace a frame; a non-tail call must consume both limits.
        for tail, status in ((True, 0), (False, 70)):
            body = ("fn recurse(n: Int) : Int\nif n == 0: 0 else: "
                    + ("" if tail else "1 + ") + "recurse(n - 1)\nend\n"
                    "fn main() : Int\nrecurse(50)\nend")
            path.write_text("module Main\n" + body + "\n")
            for flag, value in (("--max-depth", "10"), ("--max-stack", "1KiB")):
                result = run(binary, ["run", flag, value, str(path)], status)
                assert not result.stdout
                if tail:
                    assert not result.stderr
                else:
                    assert flag.encode() in result.stderr and b"Main.recurse" in result.stderr
        path.write_text("module Main\nfn main()\nend\n")
        result = run(binary, ["run", "--max-heap", "1", str(path)], 70)
        assert not result.stdout and b"--max-heap = 1" in result.stderr

        # Loading/checking failure is never run as code. Missing main is an
        # error even for check; an inference result alone is not a certificate.
        for body, code in (("fn f() : Int\n3\nend", "E0079"),
                           ('fn main() : Int\n"text"\nend', "E0078")):
            path.write_text("module Main\n" + body + "\n")
            for command in ("check", "run"):
                result = run(binary, [command, str(path)], 1)
                assert not result.stdout and code.encode() in result.stderr
            items = records(run(binary, ["check", "--json", str(path)], 1), "check")
            assert items[1]["code"] == code and items[-1]["dropped"] == 0

        # Every source stage, including lower/Core/codegen, shares the explicit
        # source stack, while the caller has only 1 MiB. Nested arithmetic also
        # checks that the running entry, not an arbitrary helper, was selected.
        path.write_text("module Main\nfn main() : Int\n" + "1 + (" * 254
                        + "1" + ")" * 254 + "\nend\n")
        for args, status in ((["run", str(path)], 255),
                             (["check", "--emit=core", str(path)], 0),
                             (["check", "--emit=bytecode", str(path)], 0)):
            result = subprocess.run([str(binary), *args], cwd=root, capture_output=True,
                                    timeout=30, preexec_fn=small_stack)
            assert result.returncode == status and not result.stderr, (args, result)

        # Program output failing is a runtime bug, unlike guide's deliberate
        # silent broken-pipe exit. It must not become SIGPIPE or tools status 1.
        path.write_text('module Main\nfn main(console: Console)\nconsole.print("hello")\nend\n')
        for args, status, reason in ((["run", str(path)], 70, b"bug: "),
                                     (["check", "--emit=core", str(path)], 1, b"E1404")):
            read_fd, write_fd = os.pipe()
            os.close(read_fd)
            try:
                result = subprocess.run([str(binary), *args], cwd=root,
                                        stdout=write_fd, stderr=subprocess.PIPE, timeout=30)
                assert result.returncode == status and reason in result.stderr, result
            finally:
                os.close(write_fd)

        # The VM's structural report is terminal-safe too, including detail
        # bytes and paths. The escaped payload is not interpreted as control.
        unsafe = pathlib.Path(directory) / "bad\u009b\u202e"
        unsafe.mkdir()
        path = unsafe / "main.mo"
        path.write_text('module Main\nfn main()\nstop "\\u{1b}\\u{009b}\\u{202e} 日本語"\nend\n')
        result = run(binary, ["run", str(path)], 70, env={**os.environ, "LC_ALL": "C"})
        result.stderr.decode("ascii")
        assert b"\\u{1b}" in result.stderr and b"\\u{9b}" in result.stderr
        assert b"\\u{202e}" in result.stderr and b"\x1b" not in result.stderr
    print(f"{binary}: real source check/run/emission, statuses, limits and defects passed")


def anchors(binary):
    # Anchor truth is not a typing certificate. Reordered arguments, opposite
    # expectations and a subject that stops expose distinct wrong consumers.
    source = ('module Main\nfn twice(n: Int) : Int\n'
              'promise "returns twice its input"\nresult == n + n\n'
              'allows result: 6, n: 3\nforbids n: 4, result: 7\nend\n'
              'if n >= 0: stop "subject must not run" else: n + n\nend\n'
              'fn main(console: Console) : Int\nconsole.print("main")\n37\nend\n')
    with tempfile.TemporaryDirectory(prefix="mo-anchors-") as directory:
        path = pathlib.Path(directory) / "main.mo"
        path.write_text(source)
        for args in (["check"], ["check", "--json"], ["check", "--emit=core"],
                     ["check", "--emit=bytecode"]):
            result = run(binary, [*args, str(path)], 0)
            assert not result.stderr
            if "--json" in args:
                records(result, "check")
            elif len(args) == 1:
                assert not result.stdout
            else:
                assert b"Main.twice.promise.1.allows.1" in result.stdout
        result = run(binary, ["run", str(path)], 37)
        assert result.stdout == b"main\n" and not result.stderr

        for allows, forbids in ((True, False), (False, True), (True, True)):
            bad = source.replace("result: 6", "result: 5") if allows else source
            if forbids:
                bad = bad.replace("result: 7", "result: 8")
            path.write_text(bad)
            items = records(run(binary, ["check", "--json", str(path)], 1), "check")
            diags = items[1:-1]
            assert len(diags) == allows + forbids
            assert all(d["code"] == "E1405" for d in diags)
            assert [d["primary"]["line"] for d in diags] == ([5] if allows else []) + ([6] if forbids else [])
            if allows:
                assert diags[0]["headline"] == "an allows anchor was rejected"
                assert diags[0]["secondary"][0]["line"] == 4
            if forbids:
                assert diags[-1]["headline"] == "a forbids anchor was accepted"
                assert not diags[-1]["secondary"]
            for args in (["check"], ["run"], ["check", "--emit=core"],
                         ["check", "--emit=bytecode"]):
                result = run(binary, [*args, str(path)], 1)
                assert not result.stdout and b"E1405" in result.stderr

        # A second false condition must retain its own source line, while a
        # rejected forbids may short-circuit before a later stop.
        path.write_text(source.replace("result == n + n\n", "result == n + n\nn < 2\n"))
        items = records(run(binary, ["check", "--json", str(path)], 1), "check")
        assert items[1]["primary"]["line"] == 6 and items[1]["secondary"][0]["line"] == 5
        path.write_text(source.replace("result == n + n\n",
                                       "result == n + n\nn == 3 or 1 / 0 == 0\n"))
        records(run(binary, ["check", "--json", str(path)], 0), "check")

        for operand, line, kind in (("n: 3", 5, "allows"), ("n: 4", 6, "forbids")):
            path.write_text(source.replace(operand, "n: 1 / 0"))
            items = records(run(binary, ["check", "--json", str(path)], 1), "check")
            assert items[1]["code"] == "E1405" and items[1]["primary"]["line"] == line
            assert items[1]["headline"] == "an anchor could not be checked"
            assert "dividing by zero" in items[1]["primary"]["label"]
            assert f"Main.twice.promise.1.{kind}.1" in items[1]["primary"]["label"]
            result = run(binary, ["run", str(path)], 1)
            assert not result.stdout and b"could not be checked" in result.stderr

        # Pure infinite computation is a compiler work refusal, not an accepted
        # anchor. The subject's and main's runtime semantics are unchanged.
        for operand in ("n: 3", "n: 4"):
            path.write_text(source.replace(operand, "n: spin()")
                            + "fn spin() : Int\nwhile true\nend\n0\nend\n")
            items = records(run(binary, ["check", "--json", str(path)], 1), "check")
            assert items[1]["headline"] == "an anchor could not be checked"
            assert "instruction budget exhausted" in items[1]["primary"]["label"]

        # This non-tail computation needs more depth than an anchor's own
        # tail call of its promise. A bound must stop certification before main.
        path.write_text(source.replace("n: 3", "n: descend(3)")
                        + "fn descend(n: Int) : Int\nif n == 0: 0 else: 1 + descend(n - 1)\nend\n")
        records(run(binary, ["check", "--json", str(path)], 0), "check")
        for flag in ("--max-stack", "--max-depth", "--max-heap"):
            result = run(binary, ["run", flag, "1", str(path)], 1)
            assert not result.stdout and b"E1405" in result.stderr
            assert b"could not be checked" in result.stderr

        # Both concrete promise instances must be certified. Ignoring the
        # Decimal instance or attaching its diagnostic to the Int anchor fails.
        generic = (source.replace("fn twice(n: Int) : Int\n",
                                  "fn twice(n: T) : T where T: Number, T: Equal, T: Order\n")
                   .replace("allows result: 6, n: 3\n",
                            "allows result: 6, n: 3\nallows n: 1.25, result: 2.5\n")
                   .replace("forbids n: 4, result: 7\n",
                            "forbids n: 4, result: 7\nforbids result: 2.0, n: 1.25\n"))
        path.write_text(generic)
        records(run(binary, ["check", "--json", str(path)], 0), "check")
        result = run(binary, ["run", str(path)], 37)
        assert result.stdout == b"main\n" and not result.stderr
        for kind in ("core", "bytecode"):
            result = run(binary, ["check", f"--emit={kind}", str(path)], 0)
            assert b"Main.twice(Decimal).promise.1" in result.stdout and not result.stderr

        path.write_text(generic.replace("result: 2.5", "result: 2.0")
                        .replace("result: 7", "result: 8"))
        items = records(run(binary, ["check", "--json", str(path)], 1), "check")
        assert [d["code"] for d in items[1:-1]] == ["E1405", "E1405"]
        assert [d["primary"]["line"] for d in items[1:-1]] == [6, 7]
        assert items[1]["secondary"][0]["line"] == 4 and not items[2]["secondary"]
        for args in (["run"], ["check", "--emit=core"], ["check", "--emit=bytecode"]):
            result = run(binary, [*args, str(path)], 1)
            assert not result.stdout and b"E1405" in result.stderr

        # Checking types examples/tests but does not test their truth or run main.
        path.write_text('module Main\nfn main()\nexamples\nfalse\nend\n'
                        'stop "main must not run"\nend\ntest "not run by check"\nassert false\nend\n')
        result = run(binary, ["check", str(path)], 0)
        assert not result.stdout and not result.stderr
        records(run(binary, ["check", "--json", str(path)], 0), "check")
    print(f"{binary}: real source anchor truth, stops, bounds and nonexecution passed")


def filenames(binary):
    # The operating system accepts byte paths. Do not repair them into Text or
    # confuse filename indices with ordinary string-table indices.
    with tempfile.TemporaryDirectory(prefix="mo-filenames-") as directory:
        reports = []
        for raw, escaped in ((b"\xff", b"\\xff"), (b"\xc3", b"\\xc3"),
                             ("ÿ".encode(), "ÿ".encode()),
                             (b"\\xff", b"\\\\xff"), (b"line\n\\", b"line\\n\\\\")):
            folder = os.fsencode(directory) + b"/" + raw
            os.mkdir(folder)
            path = folder + b"/main.mo"
            with open(path, "wb") as file:
                file.write(b"module Main\nfn main() : Int\n37\nend\n")
            result = run(binary, ["check", path], 0)
            assert not result.stdout and not result.stderr
            result = run(binary, ["run", path], 37)
            assert not result.stdout and not result.stderr
            result = run(binary, ["check", "--emit=bytecode", path], 0)
            assert not result.stderr and path.hex().encode() in result.stdout
            emitted = pathlib.Path(directory) / "program.bytecode"
            emitted.write_bytes(result.stdout)
            run(binary.parent / "tests/test_cli.test", ["--roundtrip-bytecode", str(emitted)], 0)

            with open(path, "wb") as file:
                file.write(b'module Main\nfn main()\nstop "chosen stop"\nend\n')
            result = run(binary, ["run", path], 70,
                         env={**os.environ, "LC_ALL": utf8_locale})
            result.stderr.decode("utf-8")
            assert not result.stdout and b"chosen stop" in result.stderr
            assert b"/" + escaped + b"/main.mo:" in result.stderr
            reports.append(result.stderr)

            # Anchor diagnostics must find both locations in the retained
            # source store after codegen/load, even for invalid UTF-8 paths.
            source = (b'module Main\nfn f(n: Int) : Int\npromise "identity"\n'
                      b'result == n\nallows n: 3, result: 4\nforbids n: 3, result: 2\n'
                      b'end\nn\nend\nfn main()\nend\n')
            with open(path, "wb") as file:
                file.write(source)
            items = records(run(binary, ["check", "--json", path], 1), "check")
            diag = items[1]
            assert diag["code"] == "E1405" and diag["primary"]["line"] == 5
            assert diag["primary"]["file"].endswith("/main.mo")
            assert diag["primary"]["start"] == source.index(b"allows")
            assert diag["secondary"][0]["line"] == 4
            assert diag["secondary"][0]["start"] == source.index(b"result == n")
        assert len(set(reports)) == len(reports)
    print(f"{binary}: exact filename-byte execution, bytecode and anchor locations passed")


def source_stack(binary, folder):
    # This fixture exercises the entire precedence ladder at the source-depth
    # boundary, not only nested brackets. The process stack stays small while
    # loading, parsing and whole checking use the explicit source-work stack.
    path = folder / "main.mo"
    header = ("module Main\nfn run(a: Bool, b: Bool, c: Bool, d: Range, e: Int, "
              "f: Int, g: Int, h: fn(Bool) : Int) : Bool\n")

    for depth, status in ((255, 0), (256, 1)):
        path.write_text(header + "a implies b or c and not d == e .. f + g * -h(" * depth
                        + "a" + ")" * depth + "\nend\nfn main()\nend\n")
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
    faults = binary.parent / "tests/test_cli_faults.test"
    runner(binary)
    anchors(binary)
    filenames(binary)
    items = records(run(faults, ["--json"], 1), "check")
    assert items[1]["code"] == "E1401" and items[-1]["dropped"] == 0
    assert items[1]["primary"]["file"] is None and items[1]["section"] is None
    failure = run(faults, ["--text"], 1)
    assert not failure.stdout and b"E1401" in failure.stderr
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
    for command in ("test", "fix"):
        result = run(binary, [command, good], 1)
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
                run(binary, args, 1 if command == "test" else 0)
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
    run(binary, ["run", good, "--bad", ""], 0)
    run(binary, ["test", "--only", "a", "--only", "b", "--in", "Main", good], 1)
    run(binary, ["format", good, good], 0)
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
    run(binary, ["run", good, "x" * 5000], 0)
    run(binary, ["run", good, *(str(i) for i in range(300))], 0)
    run(binary, ["run", good, *("x" * 4096 for _ in range(16))], 0)
    for command in ("format", "fix"):
        run(binary, [command, *([good] * 4096)], 0 if command == "format" else 1)
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
        encoded = folder / "caf\u00e9-ascii"
        encoded.mkdir()
        path = encoded / "main.mo"
        path.write_text('module Main\nfn run() : Int\n"Jos\u00e9 \u65e5\u672c\u8a9e \U0001f600\u00db"\nend\n',
                        encoding="utf-8")
        for terminal in ("C", "POSIX", "mo_locale_that_does_not_exist"):
            result = subprocess.run([str(binary), "check", str(path)], cwd=root,
                                    env={**os.environ, "LC_ALL": terminal},
                                    capture_output=True, timeout=30)
            assert result.returncode == 1 and not result.stdout
            result.stderr.decode("ascii")
            assert b"caf\\u{0e9}" in result.stderr and b"Jos\\u{0e9}" in result.stderr
        unsafe = folder / "bad\u009b\u202e\U000e0001"
        unsafe.mkdir()
        path = unsafe / "main.mo"
        path.write_text("module Main\nfn run()\nunknown\nend\n")
        result = run(binary, ["check", "--json", str(path)], 1)
        assert b"\\u009b\\u202e\\udb40\\udc01" in result.stdout
        assert records(result, "check")[1]["primary"]["file"] == str(path)
        unicode_folder = folder / "café"
        unicode_folder.mkdir()
        unicode_path = unicode_folder / "main.mo"
        unicode_path.write_text('module Main\nfn run() : Text\n  -"José 日本語 😀"\nend\n',
                                encoding="utf-8")
        result = run(binary, ["check", str(unicode_path)], 1,
                     env={**os.environ, "LC_ALL": utf8_locale})
        assert not result.stdout
        assert str(unicode_path).encode("utf-8") in result.stderr
        assert '-"José 日本語 😀"'.encode("utf-8") in result.stderr
        d = records(run(binary, ["check", "--json", str(unicode_path)], 1), "check")[1]
        assert d["primary"]["file"] == str(unicode_path)
        # The string occupies twelve original characters, not its byte length
        # or displayed width. Unary minus refuses this Text operand.
        assert d["primary"]["column"] == 4 and d["primary"]["end_column"] == 16
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
        # The printer must refuse rather than introduce a lexer limit error.
        for letter in (b'a', '界'.encode()):
            for length in (65532, 65533):
                source = (b'module Main\nfn f() : Text\n"' + letter * length
                          + b'"\nend\nfn main()\nend\n')
                path.write_bytes(source)
                result = run(binary, ["format", str(path)], 0 if length == 65532 else 1)
                assert not result.stdout
                if length == 65533:
                    assert b"E1403" in result.stderr and path.read_bytes() == source
                else:
                    assert not result.stderr and b'\n  "' in path.read_bytes()
                    run(binary, ["check", str(path)], 0)
        # Alignment adds 49 spaces to the short code row. Each input row is
        # legal; only the aligned result crosses the scalar limit in case two.
        for length in (65479, 65480):
            source = (b'module Main\na=0 #' + b'c' * length + b'\n' +
                      b'b' * 50 + b'=0 # short\n')
            path.write_bytes(source)
            result = run(binary, ["format", str(path)], 0 if length == 65479 else 1)
            assert not result.stdout
            if length == 65480:
                assert b'E1403' in result.stderr and path.read_bytes() == source
            else:
                expected = (b'module Main\na = 0' + b' ' * 51 + b'#' + b'c' * length +
                            b'\n' + b'b' * 50 + b' = 0  # short\n')
                assert not result.stderr and path.read_bytes() == expected
                # Constant checking is explicitly staged by the whole checker;
                # formatting may not turn that into a successful certificate.
                result = run(binary, ["check", str(path)], 1)
                assert b'E1103' in result.stderr
        # Rename is observable: an existing reader and hard link keep the old
        # complete file, while the requested path gets the complete new file.
        source = b'module Main\n\n\nfn f()\nvar x=1\nx=x+2\nx\nend\n'
        expected = b'module Main\n\nfn f()\n  var x = 1\n  x += 2\n  x\nend\n'
        path.write_bytes(source)
        path.chmod(0o640)
        alias = folder / 'alias.mo'
        os.link(path, alias)
        with path.open('rb') as old_reader:
            result = run(binary, ["format", str(path)], 0)
            assert not result.stdout and not result.stderr
            assert path.read_bytes() == expected and old_reader.read() == source
        assert alias.read_bytes() == source
        assert stat.S_IMODE(path.stat().st_mode) == 0o640
        before = path.stat()
        run(binary, ["format", str(path)], 0)
        after = path.stat()
        assert before.st_ino == after.st_ino and before.st_mtime_ns == after.st_mtime_ns
        bad = folder / 'bad.mo'
        bad_source = b'module Bad\nfn f()\n(\nend\n'
        bad.write_bytes(bad_source)
        path.write_bytes(source)
        result = run(binary, ["format", str(bad), str(path)], 1)
        assert not result.stdout and b'E' in result.stderr
        assert bad.read_bytes() == bad_source and path.read_bytes() == expected
        symlink = folder / 'linked.mo'
        symlink.symlink_to(alias)
        fifo = folder / 'pipe.mo'
        os.mkfifo(fifo)
        result = run(binary, ["format", str(symlink), str(fifo), str(folder)], 1)
        assert result.stderr.count(b'E1402') == 3 and not result.stdout
        assert symlink.is_symlink() and alias.read_bytes() == source
        assert not list(folder.glob('.mo-format-*'))
    print(f"{binary}: CLI goldens, argument matrix and JSON validity passed")


for executable in sys.argv[1:]:
    check(pathlib.Path(executable).resolve())
