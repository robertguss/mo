#!/usr/bin/env python3
"""Phase 3 Slice 3C verification harness for the unified pure-Rust `mo` CLI (`crates/mo-cli` + `crates/mo-test`).

Run from the repository root:
    python3 ci/check_rust_cli.py [--mo-bin PATH] [--modb-dir PATH] [--kill-count N]
                                 [--skip-build] [--skip-format-corpus] [--skip-upstream]

Checks every Slice-3B & Slice-3C contract directly on `target/release/mo` (with ZERO C binaries in the loop):
  1. Dynamic linkage audit (`ldd`): zero `libssl`, zero `libcrypto`, zero `libsqlite3`.
  2. Version (`--version`, `-V`), embedded docs (`guide`, `guide --reference`, `EPIPE` exit 0).
  3. CLI goldens & full flag/argument matrix (`ci/check_cli.py` Slice-3B + Slice-3C surface):
       - All 8 `tests/cli/{parse,load,modules,signatures,bodies,program,fix,multiline}/{text,json}.golden`
       - `tests/cli/good`, `tests/cli/library`, `tests/cli/test/list.golden`, and ALL 6 records of
         `tests/cli/test/json.golden` (including `Main.double.promise.1`) byte-for-byte
       - Full 44-flag acceptance/duplicate/limit/operand/budget matrix & non-UTF-8 escaping
       - Locale UTF-8/ASCII (`LC_ALL=C`, `POSIX`, invalid), unsafe JSON escapes, non-UTF-8 OS path
         bytes (`bad\\udcff`, `filenames` across `check`, `run`, `--emit=bytecode`, defect, `E1405`)
       - `source_stack` under `RLIMIT_STACK = 1 MiB`
       - `anchors`, `lower_source_fixtures`, `install_command`, `fix_command`
       - `cli_promises` (all 11 suites), `cli_intent` (`promise_selection`, `recovery_checks`,
         `service_promise_checks`, `strength_checks`, `fix_command`), `cli_gates.mutants`, and
         `cli_programs` (`observations`, `captures`, `generic_imports`, `imported_constants`, `imports`)
       - `emit`, `run` (source + `.bc`, actors, broken pipe -> 70, `LC_ALL=C` defect escaping),
         `format` (`--stdin`, `E1402`, `E1403`, atomic rename/hardlink/mode/idempotency),
         and `test` (pass/fail/equality `left`/`right`/stopped variables 4094 vs 4095, `E1406`)
  4. Complete `tests/errors/` suite across all 235 cases (0 silently skipped):
       - 226 Slice-3B + Slice-3C cases (`215 check` + `e1400`, `e1400-simulation`, `e1402`, `e1403`,
         `e1404`, `e1406`, `e1407`, `e1408`, `e1432`, `e1433`, `e1439`) verified byte-for-byte
       - 9 explicitly classified Slice 3D/3E deferred cases verified accounted for
  5. Full corpus `mo format` audit across all tracked `.mo` files (idempotency, refused unchanged,
     `check --json` preserved, `run` preserved, all 238/238 golden cases preserved).
  6. Unmodified upstream harnesses directly on `target/release/mo`:
       - `ci/check_golden.py` (238/238)
       - `ci/check_lib.py` (30/30)
       - `ci/check_programs.py` (16/16)
       - `ci/check_simulation.py`
       - `ci/check_tools.py`
       - `ci/check_crypto.py`
       - `ci/check_network.py`
       - `ci/check_reference_examples.py`
       - `bench/v0/grade.py --check --mo`
       - `ci/check_check_time.py` (run sequentially alone at the end; < 0.100 s on all 12 samples)
  7. Optional `--modb-dir` real-world gate: all MoDB test suites via `mo test`, `bench-modb.mo`,
     and `bench/kill_loop.py` crash-survival loop.
"""

import argparse
import errno
import json
import os
import pathlib
import re
import shutil
import stat
import subprocess
import sys
import tempfile
from collections import Counter

sys.dont_write_bytecode = True

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "ci"))

from cli_common import records, run, utf8_locale  # noqa: E402
from cli_fixtures import anchors, lower_source_fixtures, source_stack  # noqa: E402
from cli_gates import mutants  # noqa: E402
from cli_guide import flags, install_command  # noqa: E402
from cli_intent import (  # noqa: E402
    fix_command,
    promise_selection,
    recovery_checks,
    service_promise_checks,
    strength_checks,
)
from cli_programs import (  # noqa: E402
    captures,
    generic_imports,
    imported_constants,
    imports,
    observations,
)
import cli_promises  # noqa: E402

# Explicit disposition of the 9 non-Slice-3C `tests/errors/` folders (zero silently skipped).
DEFERRED_ERROR_CASES: dict[str, str] = {
    # Slice 3D: crates/mo-service (serve, fetch, replay, query, history)
    "e1409": "Slice 3D (mo serve)",
    "e1409-data": "Slice 3D (mo serve --data)",
    "e1409-token": "Slice 3D (mo serve token)",
    "e1410": "Slice 3D (mo query)",
    "e1411": "Slice 3D (mo query)",
    "e1411-function": "Slice 3D (mo query)",
    "e1412": "Slice 3D (mo query)",
    "e1413": "Slice 3D (mo query)",
    # Slice 3E: crates/mo-upgrade (emit --bundle, push, rehearse, diff)
    "e1418": "Slice 3E (emit --bundle)",
}

FORBIDDEN_LIBS = ("libssl", "libcrypto", "libsqlite3")


def find_or_build_rust_mo(explicit: str | None, skip_build: bool) -> pathlib.Path:
    if explicit:
        path = pathlib.Path(explicit).resolve()
        assert path.is_file(), f"mo binary not found: {path}"
        return path
    target = ROOT / "target/release/mo"
    helper_deadline = ROOT / "target/release/mo-test-deadline"
    helper_cli = ROOT / "target/release/mo-test-cli"
    if not skip_build or not target.is_file() or not helper_deadline.is_file() or not helper_cli.is_file():
        subprocess.run(
            ["cargo", "build", "--release", "--bin", "mo", "--bin", "mo-test-deadline", "--bin", "mo-test-cli"],
            cwd=ROOT,
            check=True,
        )
    assert target.is_file(), f"failed to build {target}"
    tests_dir = target.parent / "tests"
    tests_dir.mkdir(exist_ok=True)
    shutil.copy2(helper_deadline, tests_dir / "test_deadline.test")
    shutil.copy2(helper_cli, tests_dir / "test_cli.test")
    return target


def check_linkage(binary: pathlib.Path) -> None:
    ldd = subprocess.run(["ldd", str(binary)], capture_output=True, text=True, check=True)
    for lib in FORBIDDEN_LIBS:
        assert lib not in ldd.stdout, f"{binary} unexpectedly links {lib}:\n{ldd.stdout}"
    print(f"{binary}: dynamic linkage clean (zero OpenSSL / zero system libsqlite3)")


def check_cli_goldens_and_matrix(binary: pathlib.Path) -> None:
    # 1. Version & embedded docs (+ broken pipe exit 0).
    for flag in ("--version", "-V"):
        res = run(binary, [flag], 0)
        assert res.stdout == b"mo 0.3.0\n" and not res.stderr, res

    assert run(binary, ["guide", "--reference"], 0).stdout == (
        ROOT / "docs/reference/README.md"
    ).read_bytes()
    assert run(binary, ["guide"], 0).stdout == (ROOT / "docs/agent-guide.md").read_bytes()

    read_fd, write_fd = os.pipe()
    os.close(read_fd)
    try:
        res = subprocess.run(
            [str(binary), "guide", "--reference"],
            stdout=write_fd,
            stderr=subprocess.PIPE,
            timeout=30,
        )
        assert res.returncode == 0 and not res.stderr, res
    finally:
        os.close(write_fd)

    # 2. All 8 front-end stage goldens (text & JSON byte-for-byte).
    for stage in ("parse", "load", "modules", "signatures", "bodies", "program", "fix", "multiline"):
        path = f"tests/cli/{stage}/main.mo"
        for json_mode in (False, True):
            result = run(binary, ["check", *(["--json"] if json_mode else []), path], 1)
            actual = result.stdout if json_mode else result.stderr
            expected = ROOT / f"tests/cli/{stage}/{'json' if json_mode else 'text'}.golden"
            assert actual == expected.read_bytes(), (binary, expected, actual)
            if json_mode:
                items = records(result, "check")
                if stage == "bodies":
                    assert [d["primary"]["line"] for d in items[1:-1]] == [3, 6]
                if stage == "fix":
                    edit = items[1]["fixes"][0]["edits"][0]
                    assert edit["text"] == "_" and edit["span"]["start"] == edit["span"]["end"] == 19
            else:
                assert not result.stdout

    # 3. Good, library, and test fixtures.
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
    assert listed_fixture.stdout == (ROOT / "tests/cli/test/list.golden").read_bytes()
    records(listed_fixture, "test")

    # Verify EVERY record of tests/cli/test/json.golden (including Main.double.promise.1) byte-for-byte.
    ran_fixture = run(binary, ["test", "--json", test_fixture], 1)
    ran_items = records(ran_fixture, "test")
    for r in ran_items[1:-1]:
        assert type(r["microseconds"]) is int and r["microseconds"] >= 0
        r["microseconds"] = 0
    golden_lines = [
        json.loads(line)
        for line in (ROOT / "tests/cli/test/json.golden").read_text().splitlines()
    ]
    assert ran_items == golden_lines, (ran_items, golden_lines)

    # 4. Complete flag & argument refusal matrix (exit 2).
    def mistake(args, message=None):
        r = run(binary, args, 2)
        assert not r.stdout and r.stderr.count(b"\n") == 1, (args, r)
        if message:
            assert r.stderr == message.encode() + b"\n", (args, r.stderr)

    mistake(["test", "--json", "--list", "--only", "No.match", good])
    for args in (
        [],
        ["unknown"],
        [""],
        ["--version", "extra"],
        ["check"],
        ["format"],
        ["fix"],
        ["run"],
        ["test"],
        ["emit"],
        ["emit", good],
        ["guide", good],
        ["check", ""],
        ["check", good, good],
        ["test", good, good],
        ["check", "--bad", good],
        ["check", "--emit=bad", good],
        ["check", "--emit", "core", good],
        ["check", "--json", "--emit=core", good],
        ["check", "--version", good],
        ["check", "--json", "--bad", good],
        ["serve"],
        ["serve", good, good, "--port", "0", "--data", "state"],
        ["serve", "--json", good, "--port", "0", "--data", "state"],
    ):
        mistake(args)

    mistake(
        ["serve", good, "--port", "65536", "--data", "state"],
        'invalid port (expected a decimal integer from 0 to 65535): "--port" "65536"',
    )
    mistake(
        ["serve", good, "--port", "0", "--data", "-x"],
        'flag needs a nonempty value: "--data" "-x"',
    )
    mistake(
        ["fetch"],
        "usage: mo fetch --admin-socket PATH --export state|failure ID --out FILE",
    )
    mistake(
        ["fetch", "--admin-socket", "admin.sock", "--export", "state"],
        "usage: mo fetch --admin-socket PATH --export state|failure ID --out FILE",
    )
    mistake(
        ["fetch", "--admin-socket", "admin.sock", "--export", "failure", "--out", "unused"],
        "failure id must be positive",
    )
    for fid in ("0", "", "x1", "9223372036854775808"):
        mistake(
            ["fetch", "--admin-socket", "admin.sock", "--export", "failure", fid, "--out", "u"],
            "failure id must be positive",
        )
    mistake(
        ["serve", good, "--port", "0", "--data", "state", "--allow-host", "a.example", "--allow-host", "b.example"],
        'flag may be given only once (except --only): "--allow-host"',
    )
    mistake(
        ["bad\n\t\xff"],
        'unknown command (expected check, run, test, format, fix, guide, emit, serve, '
        'fetch, push, replay, query, rehearse, history, diff or install): "bad\\x0a\\x09\\xc3\\xbf"',
    )
    mistake(
        [],
        "expected a command: check, run, test, format, fix, guide, emit, serve, "
        "fetch, push, replay, query, rehearse, history, diff or install",
    )
    mistake(
        ["push", "main.mo"],
        'usage: mo push FILE --admin-socket PATH [--base DIGEST] [--migrate Module.function] [--json]: "push"',
    )
    mistake(["diff"], 'command needs a file: "diff"')
    mistake(["diff", good, good, good], f'diff takes one or two files: "{good}"')
    for args, message in (
        (["replay", good, "--data", "state"], 'replay needs --failure ID: "replay"'),
        (["replay", good, "--failure", "1"], 'give exactly one of --data DIR and --export ARTIFACT: "replay"'),
        (["replay", good, "--failure", "1", "--data", "d", "--export", "a"], 'give exactly one of --data DIR and --export ARTIFACT: "replay"'),
        (["replay", good, "--failure", "0", "--data", "d"], 'failure id must be positive: "--failure" "0"'),
        (["replay", good, "--failure", "1", "--export", "a", "--recover"], '--recover reads a data folder, not --export or --admin-socket: "--recover"'),
        (["replay", good, "--failure", "1", "--failure", "1", "--data", "d"], 'flag may be given only once (except --only): "--failure"'),
        (["query", good, "--export", "a"], 'query needs a file and a function: "query"'),
        (["query", good, "f", "g", "--export", "a"], 'query takes exactly one file and one function: "g"'),
        (["query", good, "f", "--data", "d"], 'a query over a data folder needs --failure ID: "query"'),
        (["query", good, "f"], 'give exactly one of --data DIR, --export ARTIFACT and --admin-socket PATH: "query"'),
        (["query", good, "f", "--export", "a", "--admin-socket", "s.sock"], 'give exactly one of --data DIR, --export ARTIFACT and --admin-socket PATH: "query"'),
        (["query", good, "f", "--export", "a", "--fix", "r.mo"], 'flag is not accepted by this command: "--fix"'),
        (["query", good, "f", "--export", "a", "--max-work", "0"], 'invalid limit (expected a positive decimal integer; bytes may use B, KiB, MiB or GiB): "--max-work" "0"'),
        (["history"], 'history needs a data folder: "history"'),
        (["history", "a", "b"], 'history takes exactly one data folder: "b"'),
        (["history", "--max-work", "1", "d"], 'flag is not accepted by this command: "--max-work"'),
        (["replay", "--max-heap", "1MiB", good, "--failure", "1", "--data", "d"], 'flag is not accepted by this command: "--max-heap"'),
    ):
        mistake(args, message)

    # Flag acceptance matrix across Slice-3B commands.
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
                suffix = "".join(f' "{v}"' for v in missing)
                command = next(
                    c
                    for c in ("test", "serve", "replay", "query", "history", "run", "install")
                    if c in allowed
                )
                mistake([command, flag, *missing], f'flag needs a nonempty value: "{flag}"{suffix}')

    for flag in ("--max-stack", "--max-depth", "--max-heap", "--calls", "--seconds"):
        for value in ("0", "-1", "+1", " 1", "1 ", "1.5", "1KB", "1mib", "18446744073709551616"):
            mistake(["test", flag, value, good])
    mistake(
        ["test", "--max-heap", "-1", good],
        'invalid limit (expected a positive decimal integer; bytes may use B, KiB, MiB or GiB): "--max-heap" "-1"',
    )
    for flag in ("--max-depth", "--calls", "--seconds"):
        mistake(["test", flag, "4294967296", good])
        mistake(["test", flag, "1B", good])
    for flag in ("--max-stack", "--max-heap"):
        mistake(["test", flag, "18014398509481984KiB", good])
        for value in ("1", "1B", "1KiB", "1MiB", "1GiB", "18446744073709551615"):
            run(binary, ["test", flag, value, good], 2)

    # Deferred Slice 3D/3E commands parse cleanly and exit 1 with a clear message.
    for deferred_args in (
        ["serve", "--port", "0", "--data", "state"],
        ["fetch", "--admin-socket", "a.sock", "--export", "state", "--out", "o.bin"],
        ["push", good, "--admin-socket", "a.sock"],
        ["replay", good, "--failure", "1", "--data", "d"],
        ["query", good, "f", "--export", "a"],
        ["rehearse", "--artifact", "a", "--old-bundle", "b", "--candidate", "c"],
        ["history", "d"],
        ["diff", good],
    ):
        r = run(binary, deferred_args, 1)
        assert not r.stdout and b"Phase 3" in r.stderr, (deferred_args, r)

    # Option budgets & forwarded arguments.
    run(binary, ["run", good, "--bad", ""], 0)
    run(binary, ["run", good, "x" * 5000], 0)
    run(binary, ["run", good, *(str(i) for i in range(300))], 0)
    run(binary, ["test", "--only", "a", "--only", "b", "--in", "Main", good], 2)
    run(binary, ["format", good, good], 0)
    run(binary, ["fix", good, good], 0)
    run(binary, ["test", *(["--only", "x"] * 128), good], 2)
    mistake(["test", *(["--only", "x"] * 129), good])
    mistake(["test", "--only", "x" * 4097, good])
    run(binary, ["test", *(["--only", "x" * 4096] * 15), good], 2)
    mistake(["test", *(["--only", "x" * 4096] * 16), good])

    # Format --stdin contract.
    mistake(["format", "--stdin", good], 'format --stdin takes no files: "--stdin"')
    unformatted = b"module Main\n\nfn add(a:Int,b:Int):Int\na+b\nend\n"
    expected_fmt = b"module Main\n\nfn add(a: Int, b: Int) : Int\n  a + b\nend\n"
    fmt_ok = subprocess.run(
        [str(binary), "format", "--stdin"], input=unformatted, capture_output=True, timeout=30
    )
    assert fmt_ok.returncode == 0 and fmt_ok.stdout == expected_fmt and not fmt_ok.stderr, fmt_ok
    fmt_bad = subprocess.run(
        [str(binary), "format", "--stdin"],
        input=b"module Main\nfn broken(\n",
        capture_output=True,
        timeout=30,
    )
    assert fmt_bad.returncode == 1 and not fmt_bad.stdout and b"<stdin>" in fmt_bad.stderr, fmt_bad

    print(f"{binary}: version, guide, 8 stage goldens, test goldens, and 44-flag matrix passed")


def check_locale_paths_format_and_stack(binary: pathlib.Path) -> None:
    records(run(binary, ["check", "--json", "tests/cli/missing.mo"], 1), "check")
    with tempfile.TemporaryDirectory(prefix="mo-rust-cli-paths-") as temp:
        folder = pathlib.Path(temp)
        source_stack(binary, folder)

        # Locale ASCII vs UTF-8 display.
        encoded = folder / "caf\u00e9-ascii"
        encoded.mkdir()
        path = encoded / "main.mo"
        path.write_text(
            'module Main\nfn run() : Int\n"Jos\u00e9 \u65e5\u672c\u8a9e \U0001f600\u00db"\nend\n',
            encoding="utf-8",
        )
        for terminal in ("C", "POSIX", "mo_locale_that_does_not_exist"):
            res = subprocess.run(
                [str(binary), "check", str(path)],
                cwd=ROOT,
                env={**os.environ, "LC_ALL": terminal},
                capture_output=True,
                timeout=30,
            )
            assert res.returncode == 1 and not res.stdout, res
            res.stderr.decode("ascii")
            assert b"caf\\u{0e9}" in res.stderr and b"Jos\\u{0e9}" in res.stderr

        unsafe = folder / "bad\u009b\u202e\U000e0001"
        unsafe.mkdir()
        path = unsafe / "main.mo"
        path.write_text("module Main\nfn run()\nunknown\nend\n")
        res = run(binary, ["check", "--json", str(path)], 1)
        assert b"\\u009b\\u202e\\udb40\\udc01" in res.stdout
        assert records(res, "check")[1]["primary"]["file"] == str(path)
        assert records(res, "check")[1]["primary"]["file_bytes"] == os.fsencode(path).hex()

        unicode_folder = folder / "café"
        unicode_folder.mkdir()
        unicode_path = unicode_folder / "main.mo"
        unicode_path.write_text(
            'module Main\nfn run() : Text\n  -"José 日本語 😀"\nend\n',
            encoding="utf-8",
        )
        res = run(binary, ["check", str(unicode_path)], 1, env={**os.environ, "LC_ALL": utf8_locale})
        assert not res.stdout
        assert str(unicode_path).encode("utf-8") in res.stderr
        assert '-"José 日本語 😀"'.encode("utf-8") in res.stderr
        d = records(run(binary, ["check", "--json", str(unicode_path)], 1), "check")[1]
        assert d["primary"]["column"] == 4 and d["primary"]["end_column"] == 16

        # Invalid UTF-8 source bytes.
        path = folder / "main.mo"
        path.write_bytes(b"module Main\n\xff\n")
        d = records(run(binary, ["check", "--json", str(path)], 1), "check")[1]
        assert d["code"] == "E0001" and d["primary"]["line"] == 2

        # Non-UTF-8 OS path bytes across check, run, --emit=bytecode, defect, and E1405 anchor.
        reports = []
        for raw, escaped in (
            (b"\xff", b"\\xff"),
            (b"\xc3", b"\\xc3"),
            ("ÿ".encode(), "ÿ".encode()),
            (b"\\xff", b"\\\\xff"),
            (b"line\n\\", b"line\\n\\\\"),
        ):
            sub = os.fsencode(folder) + b"/fn-" + raw
            try:
                os.mkdir(sub)
            except OSError as error:
                if error.errno != errno.EILSEQ:
                    raise
                continue
            p = sub + b"/main.mo"
            with open(p, "wb") as f:
                f.write(b"module Main\nfn main() : Int\n37\nend\n")
            run(binary, ["check", p], 0)
            run(binary, ["run", p], 37)
            bc_txt = run(binary, ["check", "--emit=bytecode", p], 0)
            assert not bc_txt.stderr and p.hex().encode() in bc_txt.stdout

            with open(p, "wb") as f:
                f.write(b'module Main\nfn main()\nstop "chosen stop"\nend\n')
            stopped = run(binary, ["run", p], 70, env={**os.environ, "LC_ALL": utf8_locale})
            stopped.stderr.decode("utf-8")
            assert not stopped.stdout and b"chosen stop" in stopped.stderr
            assert b"/fn-" + escaped + b"/main.mo:" in stopped.stderr
            reports.append(stopped.stderr)

            anchor_src = (
                b'module Main\nfn f(n: Int) : Int\npromise "identity"\n'
                b"result == n\nallows n: 3, result: 4\nforbids n: 3, result: 2\n"
                b"end\nn\nend\nfn main()\nend\n"
            )
            with open(p, "wb") as f:
                f.write(anchor_src)
            items = records(run(binary, ["check", "--json", p], 1), "check")
            diag = items[1]
            assert diag["code"] == "E1405" and diag["primary"]["line"] == 5
            assert diag["primary"]["file_bytes"] == p.hex()
            assert diag["primary"]["start"] == anchor_src.index(b"allows")
            assert diag["secondary"][0]["file_bytes"] == p.hex()
            assert diag["secondary"][0]["start"] == anchor_src.index(b"result == n")
        assert len(set(reports)) == len(reports)

        # Format limits, atomic replacement, hardlink preservation, symlink/FIFO refusal.
        path = folder / "main.mo"
        path.write_text("module Main\n" + "".join(f"fn f{i}()\nunknown\nend\n" for i in range(1500)))
        items = records(run(binary, ["check", "--json", str(path)], 1), "check")
        assert items[-1]["errors"] == 1000 and items[-1]["dropped"] == 500
        run(binary, ["format", "--", "-file.mo"], 1)

        for letter in (b"a", "界".encode()):
            for length in (65532, 65533):
                src = b'module Main\nfn f() : Text\n"' + letter * length + b'"\nend\nfn main()\nend\n'
                path.write_bytes(src)
                res = run(binary, ["format", str(path)], 0 if length == 65532 else 1)
                assert not res.stdout
                if length == 65533:
                    assert b"E1403" in res.stderr and path.read_bytes() == src
                else:
                    assert not res.stderr and b'\n  "' in path.read_bytes()
                    run(binary, ["check", str(path)], 0)

        src = b"module Main\n\n\nfn f()\nvar x=1\nx=x+2\nx\nend\n"
        expected = b"module Main\n\nfn f()\n  var x = 1\n  x += 2\n  x\nend\n"
        path.write_bytes(src)
        path.chmod(0o640)
        alias = folder / "alias.mo"
        os.link(path, alias)
        with path.open("rb") as old_reader:
            res = run(binary, ["format", str(path)], 0)
            assert not res.stdout and not res.stderr
            assert path.read_bytes() == expected and old_reader.read() == src
        assert alias.read_bytes() == src
        assert stat.S_IMODE(path.stat().st_mode) == 0o640
        before = path.stat()
        run(binary, ["format", str(path)], 0)
        after = path.stat()
        assert before.st_ino == after.st_ino and before.st_mtime_ns == after.st_mtime_ns

        # Symlink and FIFO refused without blocking (E1402).
        link = folder / "link.mo"
        link.symlink_to(path)
        assert b"E1402" in run(binary, ["format", str(link)], 1).stderr
        fifo = folder / "pipe.mo"
        os.mkfifo(fifo)
        assert b"E1402" in run(binary, ["format", str(fifo)], 1).stderr

    print(f"{binary}: locales, non-UTF-8 OS paths, source stack, and format contracts passed")


def check_emit_run_and_test_contracts(binary: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="mo-rust-cli-exec-") as temp:
        folder = pathlib.Path(temp)

        # 1. Emit & run .bc + refuse overwriting source + /dev/full + --root/--state-dir.
        src = folder / "main.mo"
        src.write_text(
            'module Main\nfn main(args: Args, console: Console) : Int\n'
            '  console.print("args=#{args.list.size}")\n'
            '  17\n'
            'end\n'
        )
        bc = folder / "app.bc"
        res = run(binary, ["emit", str(src), "-o", str(bc)], 0)
        assert not res.stdout and not res.stderr and bc.read_bytes().startswith(b"MOBC")

        # Run by .bc extension and by MOBC magic without extension.
        r1 = run(binary, ["run", str(bc), "a", "b"], 17)
        assert r1.stdout == b"args=2\n" and not r1.stderr
        magic_only = folder / "noext"
        magic_only.write_bytes(bc.read_bytes())
        r2 = run(binary, ["run", str(magic_only), "x"], 17)
        assert r2.stdout == b"args=1\n" and not r2.stderr

        # Refuse overwriting source file.
        over = run(binary, ["emit", str(src), "-o", str(src)], 1)
        assert b"E1404" in over.stderr and b"can't replace a source file" in over.stderr

        # /dev/full write failure must not attempt to remove /dev/full.
        if pathlib.Path("/dev/full").exists():
            full = run(binary, ["emit", str(src), "-o", "/dev/full"], 1)
            assert b"E1404" in full.stderr and pathlib.Path("/dev/full").exists()

        # Broken pipe on Console.print -> exit 70 with bug report.
        printer = folder / "printer.mo"
        printer.write_text(
            'module Printer\nfn main(console: Console)\n  console.print("hi")\nend\n'
        )
        r_fd, w_fd = os.pipe()
        os.close(r_fd)
        try:
            broken = subprocess.run(
                [str(binary), "run", str(printer)],
                stdout=w_fd,
                stderr=subprocess.PIPE,
                timeout=30,
            )
            assert broken.returncode == 70 and b"bug:" in broken.stderr, broken
        finally:
            os.close(w_fd)

        # LC_ALL=C defect terminal escaping (ANSI / bidi / non-ASCII).
        esc = folder / "esc.mo"
        esc.write_text(
            'module Esc\nfn main()\n  stop "\\u{1b}\\u{009b}\\u{202e} 日本語"\nend\n',
            encoding="utf-8",
        )
        c_res = run(binary, ["run", str(esc)], 70, env={**os.environ, "LC_ALL": "C"})
        c_res.stderr.decode("ascii")
        assert (
            b"\\u{1b}" in c_res.stderr
            and b"\\u{9b}" in c_res.stderr
            and b"\\u{202e}" in c_res.stderr
            and b"\x1b" not in c_res.stderr
        ), c_res.stderr

        # Stopped test variable truncation boundary (4094 fits, 4095 -> <value unavailable>).
        vars_mo = folder / "vars.mo"
        vars_mo.write_text(
            'module Vars\n'
            'fn rep(n: Int) : Text\n'
            '  var out = ""\n'
            '  var i = 0\n'
            '  while i < n\n'
            '    out = "#{out}a"\n'
            '    i += 1\n'
            '  end\n'
            '  out\n'
            'end\n'
            'test "boundary"\n'
            '  fit = rep(4094)\n'
            '  big = rep(4095)\n'
            '  assert fit.byte_count == 4094\n'
            '  assert big.byte_count == 4095\n'
            '  stop "inspect"\n'
            'end\n'
        )
        t_items = records(run(binary, ["test", "--json", str(vars_mo)], 1), "test")
        vmap = {v["name"]: v["value"] for v in t_items[1]["defect"]["variables"]}
        assert vmap["fit"] == '"' + ("a" * 4094) + '"'
        assert vmap["big"] == "<value unavailable>"

    print(f"{binary}: emit, run (.bc/actors/EPIPE/LC_ALL=C), and test variable contracts passed")


def check_errors_suite(binary: pathlib.Path) -> tuple[int, int]:
    """Run all 220 Slice-3B tests/errors/ cases byte-for-byte; account for all 235 folders."""
    # Reuse generators from ci/check_errors.py without executing its top-level main().
    errors_dir = ROOT / "tests/errors"
    cases = sorted(p for p in errors_dir.iterdir() if p.is_dir())
    assert len(cases) == 235, f"expected 235 tests/errors/ folders, found {len(cases)}"

    # Load helper functions cleanly by importing source without invoking main().
    err_src = (ROOT / "ci/check_errors.py").read_text(encoding="utf-8")
    ns: dict = {"__file__": str(ROOT / "ci/check_errors.py"), "__name__": "check_errors_lib"}
    exec(compile(err_src.replace("\nmain()\n", "\n"), "ci/check_errors.py", "exec"), ns)
    run_case = ns["run_case"]

    ran = 0
    deferred = 0
    for case in cases:
        if case.name in DEFERRED_ERROR_CASES:
            deferred += 1
            continue
        result = run_case(binary, case)
        assert result.returncode == 1, (case.name, result.returncode, result.stderr)
        stdout_file = case / "stdout"
        expected_stdout = stdout_file.read_bytes() if stdout_file.exists() else b""
        assert result.stdout == expected_stdout, (case.name, result.stdout, expected_stdout)
        expected = (case / "expected").read_bytes()
        actual = result.stderr
        if (case / "library-dependent").exists():
            masked = [
                re.sub(rb" +", b" ", re.sub(rb"\d+", b"N", text)) for text in (expected, actual)
            ]
        else:
            masked = [expected, actual]
        assert masked[0] == masked[1], (
            f"{case.name}: error message differs\n--- expected\n"
            f"{expected.decode(errors='replace')}\n--- actual\n{actual.decode(errors='replace')}"
        )
        ran += 1

    assert deferred == len(DEFERRED_ERROR_CASES), (deferred, len(DEFERRED_ERROR_CASES))
    print(
        f"{binary}: tests/errors/ parity passed ({ran} Slice-3B+3C cases byte-exact, "
        f"{deferred} explicitly deferred to Slices 3D-3E, {ran + deferred}/{len(cases)} accounted)"
    )
    return ran, deferred


def check_format_corpus(binary: pathlib.Path) -> None:
    """Full-corpus `mo format` audit on pure-Rust `mo` (idempotency, check & run invariance)."""
    tracked = [
        pathlib.Path(p.decode())
        for p in subprocess.check_output(["git", "ls-files", "-z"], cwd=ROOT).split(b"\0")
        if p
    ]
    programs = [p for p in tracked if p.suffix == ".mo"]
    positions = {"start", "end", "line", "column", "end_line", "end_column"}

    def strip_pos(val):
        if isinstance(val, dict):
            return {k: strip_pos(v) for k, v in val.items() if k not in positions}
        if isinstance(val, list):
            return [strip_pos(v) for v in val]
        return val

    def invoke(folder, *args, stdin=b""):
        res = subprocess.run(
            [str(binary), *map(str, args)],
            cwd=folder,
            input=stdin,
            capture_output=True,
            timeout=120,
        )
        assert res.returncode >= 0, f"{args}: signal {-res.returncode}"
        return res

    def check_json(folder, path):
        res = invoke(folder, "check", "--json", path)
        recs = [strip_pos(json.loads(line)) for line in res.stdout.splitlines()]
        assert recs and recs[-1]["record"] == "totals", (path, res)
        assert not res.stderr, (path, res.stderr)
        return res.returncode, recs

    def norm_test(output, folder):
        return re.sub(
            rb"(?m)^((?:passed|failed|stopped|  at) [^\n]+  |"
            rb"  in the start value  |  in the interval of timer [^\n ]+  )(?:"
            + re.escape(bytes(folder) + b"/")
            + rb")?([^\n]+\.mo):\d+:\d+$",
            rb"\1\2:<position>",
            output,
        )

    def execute(folder, path, command="run", arguments=(), stdin=b""):
        with tempfile.TemporaryDirectory(prefix="mo-rust-fmt-run-") as work:
            for data in (folder / "tests/golden").glob("*/data"):
                shutil.copytree(data, pathlib.Path(work) / data.relative_to(folder), symlinks=True)
            res = invoke(work, command, folder / path, *arguments, stdin=stdin)
            written = {
                str(p.relative_to(work)): p.read_bytes()
                for p in pathlib.Path(work).rglob("*")
                if p.is_file()
            }
        if command == "run" and res.stderr.startswith(b"error E0079:"):
            assert res.returncode == 1 and not res.stdout
            return None
        errors = re.sub(rb"(?m)^(  at [^\n]*\.mo:)\d+\.\.\d+(\))$", rb"\1<span>\2", res.stderr)
        output = norm_test(res.stdout, folder) if command == "test" else res.stdout
        return res.returncode, output, errors, written

    counts = Counter()
    failures = []
    with tempfile.TemporaryDirectory(prefix="mo-rust-fmt-corpus-") as work:
        folder = pathlib.Path(work)
        for path in tracked:
            dest = folder / path
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / path, dest, follow_symlinks=False)

        before = {}
        runs = {}
        for path in programs:
            before[path] = check_json(folder, path)
            if before[path][0] == 0:
                runs[path] = execute(folder, path)

        golden = []
        for case in sorted((folder / "tests/golden").iterdir()):
            if not case.is_dir():
                continue

            def read(name, default=b""):
                f = case / name
                return f.read_bytes() if f.exists() else default

            path = (
                pathlib.Path(read("entry").decode().strip())
                if (case / "entry").exists()
                else case.relative_to(folder) / "main.mo"
            )
            command = read("command", b"run").decode().strip()
            arguments = read("args").decode().splitlines()
            stdin = read("stdin")
            expected = execute(folder, path, command, arguments, stdin)
            stdout = norm_test(read("stdout"), folder) if command == "test" else read("stdout")
            assert expected is not None and expected[:2] == (int(read("status", b"0")), stdout), (
                f"golden {case.name}: baseline mismatch: {expected!r}"
            )
            golden.append((case.name, path, command, arguments, stdin, expected))

        for path in programs:
            source = (folder / path).read_bytes()
            first = invoke(folder, "format", path)
            once = (folder / path).read_bytes()
            second = invoke(folder, "format", path)
            if (first.returncode, first.stdout, first.stderr) != (
                second.returncode,
                second.stdout,
                second.stderr,
            ) or once != (folder / path).read_bytes():
                failures.append(f"{path}: format is not idempotent")
            if first.returncode:
                counts["refused"] += 1
                if once != source:
                    failures.append(f"{path}: refused format changed the file")
                if before[path][0] == 0:
                    failures.append(f"{path}: format refused a checked program\n{first.stderr!r}")
            else:
                counts["formatted"] += 1

        for path in programs:
            after = check_json(folder, path)
            if before[path] != after:
                failures.append(f"{path}: check changed\n{before[path]!r}\n{after!r}")
            if path in runs:
                res = execute(folder, path)
                counts["no main" if runs[path] is None else "runs"] += 1
                if runs[path] != res:
                    failures.append(f"{path}: run changed\n{runs[path]!r}\n{res!r}")

        for name, path, command, arguments, stdin, expected in golden:
            res = execute(folder, path, command, arguments, stdin)
            if expected != res:
                failures.append(f"golden {name}: {command} changed\n{expected!r}\n{res!r}")

        assert not failures, "\n\n".join(failures)

    print(
        f"{binary}: format corpus passed ({len(programs)} tracked .mo files, "
        f"{counts['formatted']} formatted, {counts['refused']} unchanged refusals, "
        f"{counts['runs']} run comparisons, {counts['no main']} modules without main, "
        f"{len(golden)}/238 golden comparisons)"
    )


def check_test_engine_suites(binary: pathlib.Path) -> None:
    promise_suites = sorted(
        name
        for name in dir(cli_promises)
        if callable(getattr(cli_promises, name))
        and getattr(cli_promises, name).__module__ == "cli_promises"
        and not name.startswith("_")
    )
    assert len(promise_suites) == 11, promise_suites
    for name in promise_suites:
        getattr(cli_promises, name)(binary)
    promise_selection(binary)
    recovery_checks(binary)
    service_promise_checks(binary)
    strength_checks(binary)
    mutants(binary)
    observations(binary)
    captures(binary)
    generic_imports(binary)
    imported_constants(binary)
    imports(binary)
    print(
        f"{binary}: Slice 3C mo-test CLI suites passed "
        f"({len(promise_suites)} cli_promises, 4 cli_intent, cli_gates.mutants, 5 cli_programs)"
    )


def check_upstream_harnesses(binary: pathlib.Path) -> None:
    for cmd in (
        [sys.executable, str(ROOT / "ci/check_golden.py"), str(binary)],
        [sys.executable, str(ROOT / "ci/check_lib.py"), str(binary)],
        [sys.executable, str(ROOT / "ci/check_programs.py"), str(binary)],
        [sys.executable, str(ROOT / "ci/check_simulation.py"), str(binary)],
        [sys.executable, str(ROOT / "ci/check_tools.py"), str(binary)],
        [sys.executable, str(ROOT / "ci/check_crypto.py"), str(binary)],
        [sys.executable, str(ROOT / "ci/check_network.py"), str(binary)],
        [sys.executable, str(ROOT / "ci/check_reference_examples.py"), str(binary)],
        [sys.executable, str(ROOT / "bench/v0/grade.py"), "--check", "--mo", str(binary)],
        # Timed check runs sequentially alone at the end.
        [sys.executable, str(ROOT / "ci/check_check_time.py"), str(binary)],
    ):
        subprocess.run(cmd, cwd=ROOT, check=True)


def check_modb_gate(binary: pathlib.Path, modb_dir: pathlib.Path, kill_count: int) -> None:
    modb = modb_dir.resolve()
    assert modb.is_dir(), f"--modb-dir is not a directory: {modb}"
    suites = [
        "src/modb/codec.mo",
        "src/modb/pager.mo",
        "src/modb/tree.mo",
        "src/modb/table.mo",
        "tests/test-modb.mo",
        "tests/test-table.mo",
    ]
    total_passed = 0
    for rel in suites:
        suite_path = modb / rel
        if not suite_path.is_file():
            continue
        res = subprocess.run(
            [str(binary), "test", "--json", str(suite_path)],
            cwd=modb,
            capture_output=True,
            timeout=120,
        )
        assert res.returncode == 0 and not res.stderr, (
            f"MoDB {rel} failed (rc={res.returncode}):\n"
            f"{res.stdout.decode(errors='replace')}\n{res.stderr.decode(errors='replace')}"
        )
        recs = [json.loads(line) for line in res.stdout.decode("utf-8").splitlines()]
        totals = recs[-1]
        assert totals["record"] == "totals" and totals["failed"] == 0 and totals["stopped"] == 0
        total_passed += totals["passed"]
        print(f"MoDB {rel}: {totals['passed']} passed, 0 failed, 0 stopped")

    bench_mo = modb / "bench/bench-modb.mo"
    if bench_mo.is_file():
        with tempfile.TemporaryDirectory(prefix="mo-rust-modb-bench-") as tmp:
            res = subprocess.run(
                [str(binary), "run", "--root", tmp, str(bench_mo)],
                cwd=modb,
                capture_output=True,
                text=True,
                timeout=120,
            )
            assert res.returncode == 0, f"MoDB bench failed:\n{res.stdout}\n{res.stderr}"
            print(f"MoDB bench/bench-modb.mo: passed ({len(res.stdout.splitlines())} lines)")

    kill_py = modb / "bench/kill_loop.py"
    if kill_py.is_file() and kill_count > 0:
        res = subprocess.run(
            [sys.executable, str(kill_py), str(binary), str(kill_count)],
            cwd=modb,
            capture_output=True,
            text=True,
            timeout=240,
        )
        assert res.returncode == 0, f"MoDB kill_loop failed:\n{res.stdout}\n{res.stderr}"
        print(f"MoDB kill_loop: {res.stdout.strip().splitlines()[-1]}")

    print(f"MoDB gate passed: {total_passed} total tests passed on pure-Rust {binary}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mo-bin", help="Path to pure-Rust mo binary (default: target/release/mo)")
    parser.add_argument("--modb-dir", help="Optional path to MoDB checkout for real-world gate")
    parser.add_argument("--kill-count", type=int, default=20, help="SIGKILL loop count for MoDB")
    parser.add_argument("--skip-build", action="store_true", help="Do not rebuild target/release/mo")
    parser.add_argument("--skip-format-corpus", action="store_true", help="Skip full-corpus format audit")
    parser.add_argument("--skip-upstream", action="store_true", help="Skip unmodified upstream harnesses")
    args = parser.parse_args()

    binary = find_or_build_rust_mo(args.mo_bin, args.skip_build)
    check_linkage(binary)
    check_cli_goldens_and_matrix(binary)
    check_locale_paths_format_and_stack(binary)
    anchors(binary)
    lower_source_fixtures(binary)
    install_command(binary)
    fix_command(binary)
    check_emit_run_and_test_contracts(binary)
    check_test_engine_suites(binary)
    check_errors_suite(binary)
    if not args.skip_format_corpus:
        check_format_corpus(binary)
    if not args.skip_upstream:
        check_upstream_harnesses(binary)
    if args.modb_dir:
        check_modb_gate(binary, pathlib.Path(args.modb_dir), args.kill_count)

    print(f"check_rust_cli.py: ALL Phase 3 Slice 3C checks passed on {binary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
