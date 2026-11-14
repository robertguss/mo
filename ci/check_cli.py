#!/usr/bin/env python3
"""Black-box CLI contracts and goldens, with Python's real JSON parser."""
import errno
import json
import os
import pathlib
import stat
import subprocess
import sys
import tempfile

from cli_common import records, root, run, utf8_locale
from cli_fixtures import anchors, constants, filenames, lower_source_fixtures, source_stack
from cli_guide import emit_command, flags, guide_claims
from cli_intent import (
    fix_command, intent_command, promise_selection, recovery_checks, service_promise_checks,
    strength_checks,
)
from cli_programs import captures, generic_imports, imported_constants, imports, observations
from cli_promises import (
    collection_promises, composite_deadlines, composite_promises, fake_capability_promises,
    generic_promises, hidden_promises, promise_boundaries, promise_deadlines, promise_shrinking,
    scalar_promises,
)
from cli_runner import runner
from cli_serve import serve_command, service_commands, service_refusals


def check(binary):
    driver = binary.parent / "tests/test_cli.test"
    faults = binary.parent / "tests/test_cli_faults.test"
    guide_claims(binary)
    service_commands(binary)
    service_refusals(binary)
    serve_command(binary)
    emit_command(binary)
    runner(binary)
    lower_source_fixtures(binary)
    anchors(binary)
    constants(binary)
    filenames(binary)
    observations(binary)
    captures(binary)
    imports(binary)
    generic_imports(binary)
    scalar_promises(binary)
    promise_boundaries(binary)
    promise_deadlines(binary)
    promise_shrinking(binary)
    generic_promises(binary)
    fake_capability_promises(binary)
    hidden_promises(binary)
    composite_promises(binary)
    collection_promises(binary)
    composite_deadlines(binary)
    promise_selection(binary)
    recovery_checks(binary)
    service_promise_checks(binary)
    strength_checks(binary)
    intent_command(binary)
    imported_constants(binary)
    fix_command(binary)
    items = records(run(faults, ["--json"], 1), "check")
    assert items[1]["code"] == "E1401" and items[-1]["dropped"] == 0
    assert items[1]["primary"]["file"] is None and items[1]["primary"]["file_bytes"] is None
    assert items[1]["section"] is None
    failure = run(faults, ["--text"], 1)
    assert not failure.stdout and b"E1401" in failure.stderr
    for stage in ("parse", "load", "modules", "signatures", "bodies", "program", "fix",
                  "multiline"):
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
    library = "tests/cli/library/main.mo"
    result = run(binary, ["check", library], 0)
    assert not result.stdout and not result.stderr
    run(binary, ["check", "--emit=bytecode", library], 0)
    records(run(binary, ["check", "--json", library], 0), "check")
    assert b"E0079" in run(binary, ["run", library], 1).stderr
    tested_lib = records(run(binary, ["test", "--json", library], 0), "test")
    assert [(r["name"], r["outcome"]) for r in tested_lib[1:-1]] == [("Main.test.1", "passed")]
    test_fixture = "tests/cli/test/main.mo"
    listed_fixture = run(binary, ["test", "--json", "--list", test_fixture], 0)
    assert listed_fixture.stdout == (root / "tests/cli/test/list.golden").read_bytes()
    records(listed_fixture, "test")
    ran_fixture = run(binary, ["test", "--json", test_fixture], 1)
    ran_items = records(ran_fixture, "test")
    for r in ran_items[1:-1]:
        assert type(r["microseconds"]) is int and r["microseconds"] >= 0
        r["microseconds"] = 0
    normalized_json = "".join(json.dumps(r, separators=(",", ":")) + "\n" for r in ran_items).encode()
    assert normalized_json == (root / "tests/cli/test/json.golden").read_bytes()
    for mode in ("text", "json"):
        result = run(driver, [f"--render-{mode}"], 0)
        assert not result.stderr
        assert result.stdout == (root / f"tests/cli/render-{mode}.golden").read_bytes()
        if mode == "json":
            d = json.loads(result.stdout)
            assert d["primary"]["column"] == 2
            assert d["primary"]["file"] == 'badÿ"\\.mo'
            assert d["primary"]["file_bytes"] == b'bad\xff"\\.mo'.hex()
            assert d["secondary"][0]["line"] == 7
            assert d["secondary"][0]["file_bytes"] == b'bad\xff"\\.mo'.hex()
            assert d["secondary"][1]["file"] is None and d["secondary"][1]["file_bytes"] is None
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
    result = run(binary, ["test", "--json", "--list", "--only", "No.match", good], 2)
    assert not result.stdout and result.stderr.count(b"\n") == 1

    def mistake(args, message=None):
        r = run(binary, args, 2)
        assert not r.stdout and r.stderr.count(b"\n") == 1, (args, r)
        if message:
            assert r.stderr == message.encode() + b"\n", (args, r.stderr)

    for args in ([], ["unknown"], [""], ["--version", "extra"], ["check"], ["format"],
                 ["fix"], ["run"], ["test"], ["emit"], ["emit", good], ["guide", good], ["check", ""],
                 ["check", good, good], ["test", good, good], ["check", "--bad", good],
                 ["check", "--emit=bad", good], ["check", "--emit", "core", good],
                 ["check", "--json", "--emit=core", good], ["check", "--version", good],
                 ["check", "--json", "--bad", good], ["serve"], ["serve", good],
                 ["serve", good, "--port", "0"], ["serve", good, "--data", "state"],
                 ["serve", good, good, "--port", "0", "--data", "state"],
                 ["serve", "--json", good, "--port", "0", "--data", "state"]):
        mistake(args)
    mistake(["serve", good, "--port", "65536", "--data", "state"],
            'invalid port (expected a decimal integer from 0 to 65535): "--port" "65536"')
    mistake(["serve", good, "--port", "0", "--data", "-x"],
            'flag needs a nonempty value: "--data" "-x"')
    mistake(["fetch"], 'usage: mo fetch URL --export state|failure ID --out FILE --token-env NAME')
    mistake(["fetch", "http://localhost:80/mcp", "--export", "failure", "--out", "unused", "--token-env", "TOKEN"],
            'usage: mo fetch URL --export state|failure ID --out FILE --token-env NAME')
    for failure_id in ["0", "", "x1", "9223372036854775808"]:
        mistake(["fetch", "http://localhost:80/mcp", "--export", "failure", failure_id, "--out",
                 "unused", "--token-env", "TOKEN"], "failure id must be positive")
    mistake(["serve", good, "--data", "state"], 'serve needs --port N: "serve"')
    mistake(["serve", good, "--port", "0"], 'serve needs --data DIR: "serve"')
    mistake(["serve", good, "--port", "0", "--data", "state", "--allow-host", "a.example",
             "--allow-host", "b.example"],
            'flag may be given only once (except --only): "--allow-host"')
    mistake(["bad\n\t\xff"], 'unknown command (expected check, run, test, format, fix, guide, emit, serve, fetch, push, review, replay, query, rehearse, history or intent): "bad\\x0a\\x09\\xc3\\xbf"')
    mistake([], 'expected a command: check, run, test, format, fix, guide, emit, serve, fetch, push, review, replay, query, rehearse, history or intent')
    mistake(["push", "main.mo", "--url", "http://127.0.0.1:1/mcp"], 'usage: mo push FILE --url URL --base DIGEST --token-env NAME --upgrade-token-env NAME [--migrate Module.function] [--json]: "push"')
    for args, message in (
            (["replay", good, "--data", "state"], 'replay needs --failure ID: "replay"'),
            (["replay", good, "--failure", "1"],
             'give exactly one of --data DIR and --export ARTIFACT: "replay"'),
            (["replay", good, "--failure", "1", "--data", "d", "--export", "a"],
             'give exactly one of --data DIR and --export ARTIFACT: "replay"'),
            (["replay", good, "--failure", "0", "--data", "d"],
             'failure id must be positive: "--failure" "0"'),
            (["replay", good, "--failure", "1", "--export", "a", "--recover"],
             '--recover reads a data folder, not --export: "--recover"'),
            (["replay", good, "--failure", "1", "--failure", "1", "--data", "d"],
             'flag may be given only once (except --only): "--failure"'),
            (["query", good, "--export", "a"], 'query needs a file and a function: "query"'),
            (["query", good, "f", "g", "--export", "a"],
             'query takes exactly one file and one function: "g"'),
            (["query", good, "f", "--data", "d"],
             'a query over a data folder needs --failure ID: "query"'),
            (["query", good, "f", "--export", "a", "--fix", "r.mo"],
             'flag is not accepted by this command: "--fix"'),
            (["query", good, "f", "--export", "a", "--max-work", "0"],
             'invalid limit (expected a positive decimal integer; bytes may use B, KiB, MiB or GiB): "--max-work" "0"'),
            (["history"], 'history needs a data folder: "history"'),
            (["history", "a", "b"], 'history takes exactly one data folder: "b"'),
            (["history", "--max-work", "1", "d"], 'flag is not accepted by this command: "--max-work"'),
            (["replay", "--max-heap", "1MiB", good, "--failure", "1", "--data", "d"],
             'flag is not accepted by this command: "--max-heap"')):
        mistake(args, message)
    for flag, (value, allowed) in flags.items():
        for command in ("check", "run", "test", "format", "fix", "guide"):
            args = [command, flag, *value, *([] if command == "guide" else [good])]
            if command not in allowed:
                mistake(args, f'flag is not accepted by this command: "{flag}"')
            else:
                run(binary, args, 2 if command == "test" else 0)
                if flag != "--only":
                    mistake([command, flag, *value, flag, *value, *([] if command == "guide" else [good])])
        if value:
            for missing in ([], [""], ["--json"]):
                suffix = ''.join(f' "{v}"' for v in missing)
                command = next(c for c in ("test", "serve", "replay", "query", "history")
                               if c in allowed)
                mistake([command, flag, *missing], f'flag needs a nonempty value: "{flag}"{suffix}')
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
            run(binary, ["test", flag, value, good], 2)
    run(binary, ["run", good, "--bad", ""], 0)
    run(binary, ["test", "--only", "a", "--only", "b", "--in", "Main", good], 2)
    run(binary, ["format", good, good], 0)
    run(binary, ["fix", good, good], 0)
    for version in ("--version", "-V"):
        assert run(binary, [version], 0).stdout == b"mo 0.2.0\n"
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
    # fix checks each file's program, so it gets fewer copies than format's 4096.
    run(binary, ["format", *([good] * 4096)], 0)
    run(binary, ["fix", *([good] * 64)], 0)
    run(binary, ["test", *(["--only", "x"] * 128), good], 2)
    mistake(["test", *(["--only", "x"] * 129), good])
    mistake(["test", "--only", "x" * 4097, good])
    run(binary, ["test", *(["--only", "x" * 4096] * 15), good], 2)
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
        assert records(result, "check")[1]["primary"]["file_bytes"] == os.fsencode(path).hex()
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
        assert d["primary"]["file_bytes"] == os.fsencode(unicode_path).hex()
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
            path = invalid / "bad\udcff.mo"
            path.write_text("module Main\nfn run()\nunknown\nend\n")
            d = records(run(binary, ["check", "--json", str(path)], 1), "check")[1]
            assert "badÿ/badÿ.mo" in d["primary"]["file"]
            assert d["primary"]["file_bytes"] == os.fsencode(path).hex()
            assert d["primary"]["file_bytes"].endswith(b"bad\xff/bad\xff.mo".hex())
            valid_utf8 = folder / "badÿ" / "badÿ.mo"
            valid_utf8.parent.mkdir()
            valid_utf8.write_text("module Main\nfn run()\nunknown\nend\n")
            d_utf8 = records(run(binary, ["check", "--json", str(valid_utf8)], 1), "check")[1]
            assert d_utf8["primary"]["file"] == d["primary"]["file"]
            assert d_utf8["primary"]["file_bytes"] == os.fsencode(valid_utf8).hex()
            assert d_utf8["primary"]["file_bytes"] != d["primary"]["file_bytes"]
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
                # A constant-only library module checks cleanly (D250), while
                # running it still requires main (E0079).
                run(binary, ["check", str(path)], 0)
                result = run(binary, ["run", str(path)], 1)
                assert b'E0079' in result.stderr
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
        # Reference 12: a specification directly followed by its code is accepted, and
        # mo format writes the blank line between them.
        path.write_bytes(b'module Main\n\nfn f() : Int\n  examples\n    f() == 1\n  end\n'
                         b'  1\nend\n')
        run(binary, ["check", str(path)], 0)
        run(binary, ["format", str(path)], 0)
        assert path.read_bytes() == (b'module Main\n\nfn f() : Int\n  examples\n    f() == 1\n'
                                     b'  end\n\n  1\nend\n')
        for fragment in (b'1.', b'2.minute.', b'fn f()\n1.minutes\n', b'Int.minute('):
            incomplete = b'module Main\n' + fragment
            path.write_bytes(incomplete)
            result = run(binary, ["format", str(path)], 1)
            assert result.stderr and not result.stdout and path.read_bytes() == incomplete
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
        assert not list(folder.glob('.mo-write-*'))
    print(f"{binary}: CLI goldens, argument matrix and JSON validity passed")


if __name__ == "__main__":
    for executable in sys.argv[1:]:
        check(pathlib.Path(executable).resolve())
