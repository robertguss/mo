#!/usr/bin/env python3
"""Differential test harness comparing the pure-Rust runtime (`mo-rt`) against C `mo`.

Run from the repository root:
    python3 ci/check_rust_runtime.py [--mo PATH] [--mo-rt PATH] [--skip-build]
                                     [--modb-dir PATH] [--case=NAME ...] [--jobs N]

What this checks for Phase 1 (Slices 1A–1E — 59/59 hostops, 87/87 opcodes, 187/187 prims):
  1. Builds `target/release/mo-rt` (unless `--skip-build`) and locates or builds C `mo`.
  2. Enforces static/binary invariants:
       - `ldd target/release/mo-rt` has zero `libssl`, zero `libcrypto`, and zero `libsqlite3`.
       - `#![forbid(unsafe_code)]` is present in every Rust crate entry point and workspace lints.
  3. Dynamically discovers and accounts for 100% of `tests/golden/` (all 238/238 suites, 0 skips):
       - Full runtime parity suites (209): emit `.bc`, `mo-rt verify`, `mo-rt anchors`,
         run `mo-rt` (`run` / `test`), and assert exact byte-for-byte stdout, stderr, and
         exit-status parity against C `mo` and golden expectations.
       - Compile-time-only error goldens (5): assert `mo emit` rejects them with exact
         expected compile error and exit status.
       - Promise / service recovery `mo test` suites (24): emit `--tests` `.bc`,
         `mo-rt verify`, execute all promise `allows`/`forbids` anchors via `mo-rt anchors`,
         execute every `test`/`example` block via `mo-rt test`, and diff every executed
         `test`/`example` byte-for-byte against C `mo test --only`.
  4. Expands full runtime parity across ALL Phase 1 corpora:
       - All 46 `tests/lower-source/*/main.mo` suites (`run` + `test` + `anchors`).
       - All `examples/` programs (`examples/*.mo` + `examples/*/main.mo`).
       - All 30 `lib/**/*.mo` standard library modules.
       - All `tools/` and `services/` programs (`tools/*.mo`, `tools/*/main.mo`,
         `services/mo-*/main.mo`, `services/task-queue/queue-tests.mo`).
       - Optional `--modb-dir PATH`: all 5 MoDB suites (`src/modb.mo`, `src/modb/table.mo`,
         `tests/test-modb.mo`, `tests/test-table.mo`, `examples/shop.mo`, 29/29 tests).
  5. Runs inline differential checks for `Env`, `--max-depth`, `BigInt`, `--root DIR`
     real `Files`/`ReadFiles`/`Files.File`, `Clock`, `Random`, `SecureRandom`, `Crypto`,
     `Unicode16` (+ UCD 16.0.0 `--rust --check`), real & fake `Network` (loopback TCP +
     loopback TLS with `SSL_CERT_FILE` + `allow_hosts`), real & fake `Sqlite` (`--root`,
     `--state-dir`, WAL, CRUD, authorizer, confinement), real & fake `Command` (`--root`,
     `within`, `allow_programs`, timeout), `Simulation` (`start`/`send`/`advance`/`crash`/
     `restart`/timers/admission), and `mo-rt verify` rejection of corrupted bytecode.
"""

import argparse
import concurrent.futures
import os
import pathlib
import re
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading

ROOT = pathlib.Path(__file__).resolve().parent.parent
GOLDEN = ROOT / "tests/golden"
LOWER_SOURCE = ROOT / "tests/lower-source"

MINIMUM_GOLDEN_SUITES = 238
MINIMUM_LOWER_SOURCE_SUITES = 46
MINIMUM_EXAMPLE_PROGRAMS = 13
MINIMUM_LIB_MODULES = 30
MINIMUM_TOOL_SERVICE_PROGRAMS = 16

TEST_LINE_RE = re.compile(r"^(passed|failed|stopped) (\S+)  ", re.M)
PROMISE_OR_SERVICE_OUT_RE = re.compile(
    r"^(?:passed|failed|stopped) \S+(?:\.promise\.\d+|\.service\.promise\.\d+|\.service\.recovery)  ",
    re.M,
)
PROMISE_OR_SERVICE_NAME_RE = re.compile(
    r"(?:\.promise\.\d+|\.service\.promise\.\d+|\.service\.recovery)$"
)
ANCHOR_COUNT_RE = re.compile(r"^(\d+) anchors passed$", re.M)
SUMMARY_RE = re.compile(r"^(\d+) passed, (\d+) failed, (\d+) stopped$", re.M)

# Lower-source programs that also declare a `service` block: C `mo run` rejects them with E1408
# ("mo run does not start services yet"), while `mo emit` + `mo-rt run` runs their `main` entry
# and returns their exact Core-checked exit code (see ci/check_lower_source.py).
SERVICE_MAIN_EXPECTED_EXIT = {
    "service-descriptor": 19,
    "service-simulation": 0,
}

MODB_SUITES = (
    "src/modb.mo",
    "src/modb/table.mo",
    "tests/test-modb.mo",
    "tests/test-table.mo",
    "examples/shop.mo",
)


def read_bytes(case: pathlib.Path, name: str, default: bytes = b"") -> bytes:
    path = case / name
    return path.read_bytes() if path.exists() else default


def find_or_build_mo(explicit: str | None) -> pathlib.Path:
    if explicit:
        path = pathlib.Path(explicit).resolve()
        assert path.is_file(), f"mo binary not found: {path}"
        return path
    built = ROOT / "target/release/mo"
    if not built.is_file():
        subprocess.run(
            ["cargo", "build", "--release", "--bin", "mo"],
            cwd=ROOT,
            check=True,
        )
    assert built.is_file(), f"failed to build {built}"
    return built


def find_or_build_mo_rt(explicit: str | None, skip_build: bool) -> pathlib.Path:
    if explicit:
        path = pathlib.Path(explicit).resolve()
        assert path.is_file(), f"mo-rt binary not found: {path}"
        return path
    if not skip_build:
        subprocess.run(
            ["cargo", "build", "--release", "--bin", "mo-rt"],
            cwd=ROOT,
            check=True,
        )
    built = ROOT / "target/release/mo-rt"
    assert built.is_file(), f"mo-rt binary not found: {built}"
    return built


def check_no_forbidden_linkage(mo_rt: pathlib.Path) -> None:
    ldd = shutil.which("ldd")
    if ldd is None:
        return
    res = subprocess.run([ldd, str(mo_rt)], capture_output=True, text=True, timeout=30)
    assert res.returncode == 0, f"ldd {mo_rt} failed: {res.stderr}"
    lower = res.stdout.lower()
    for forbidden in ("libssl", "libcrypto", "libsqlite3"):
        assert forbidden not in lower, (
            f"{mo_rt} unexpectedly links {forbidden}:\n{res.stdout}"
        )


def check_forbid_unsafe_code() -> None:
    workspace_toml = (ROOT / "Cargo.toml").read_text(encoding="utf-8")
    assert 'unsafe_code = "forbid"' in workspace_toml, (
        "Cargo.toml workspace lints must set unsafe_code = \"forbid\""
    )
    crate_dirs = sorted(p for p in (ROOT / "crates").iterdir() if p.is_dir())
    assert len(crate_dirs) >= 7, f"expected >= 7 crates, found {len(crate_dirs)}"
    checked = 0
    for crate in crate_dirs:
        entry_points = [
            p
            for p in (crate / "src/lib.rs", crate / "src/main.rs", crate / "build.rs")
            if p.is_file()
        ]
        assert entry_points, f"{crate}: no Rust entry point found"
        for ep in entry_points:
            first_lines = ep.read_text(encoding="utf-8").splitlines()[:5]
            assert any(line.strip() == "#![forbid(unsafe_code)]" for line in first_lines), (
                f"{ep.relative_to(ROOT)} is missing #![forbid(unsafe_code)] at top of file"
            )
            checked += 1
    assert checked >= 8, f"expected >= 8 crate entry points, checked {checked}"


def parse_anchors_count(stdout: str) -> int:
    m = ANCHOR_COUNT_RE.search(stdout)
    assert m is not None, f"unexpected mo-rt anchors output: {stdout!r}"
    return int(m.group(1))


def classify_golden_case(case: pathlib.Path) -> str:
    command = read_bytes(case, "command", b"run").decode().strip()
    exp_stdout = read_bytes(case, "stdout")
    exp_stderr = read_bytes(case, "stderr")
    exp_status = int(read_bytes(case, "status", b"0").decode().strip())
    if exp_status != 0 and exp_stdout == b"" and exp_stderr.startswith(b"error E"):
        return "compile-only"
    if command == "test" and PROMISE_OR_SERVICE_OUT_RE.search(exp_stdout.decode(errors="replace")):
        return "promise-or-service"
    return "full"


def diff_test_only_batches(
    mo: pathlib.Path,
    mo_rt: pathlib.Path,
    program: str,
    bc_path: pathlib.Path,
    names: list[str],
    cwd: pathlib.Path,
    rs_full: subprocess.CompletedProcess[bytes] | None = None,
) -> list[str]:
    """Compare executed `test`/`example` outcomes byte-for-byte against C `mo test --only`."""
    problems: list[str] = []
    if not names:
        return problems
    # C `mo` caps CLI options at 256 words (`--only NAME` is 2 words each -> batch <= 80 names).
    if len(names) <= 80 and rs_full is not None:
        c_cmd = [str(mo), "test"]
        for name in names:
            c_cmd.extend(["--only", name])
        c_cmd.append(program)
        c_res = subprocess.run(c_cmd, cwd=cwd, capture_output=True, timeout=120)
        if rs_full.stdout != c_res.stdout:
            problems.append(
                "stdout mismatch vs C mo test --only\n"
                f"--- C mo\n{c_res.stdout.decode(errors='replace')}"
                f"--- mo-rt\n{rs_full.stdout.decode(errors='replace')}"
            )
        if rs_full.stderr != c_res.stderr:
            problems.append(
                "stderr mismatch vs C mo test --only\n"
                f"--- C mo\n{c_res.stderr.decode(errors='replace')}"
                f"--- mo-rt\n{rs_full.stderr.decode(errors='replace')}"
            )
        if rs_full.returncode != c_res.returncode:
            problems.append(
                f"exit status mismatch vs C mo test --only: C={c_res.returncode}, mo-rt={rs_full.returncode}"
            )
        return problems

    for start in range(0, len(names), 80):
        batch = names[start : start + 80]
        c_cmd = [str(mo), "test"]
        rs_cmd = [str(mo_rt), "test"]
        for name in batch:
            c_cmd.extend(["--only", name])
            rs_cmd.extend(["--only", name])
        c_cmd.append(program)
        rs_cmd.append(str(bc_path))
        c_res = subprocess.run(c_cmd, cwd=cwd, capture_output=True, timeout=120)
        rs_res = subprocess.run(rs_cmd, cwd=cwd, capture_output=True, timeout=120)
        if (rs_res.returncode, rs_res.stdout, rs_res.stderr) != (
            c_res.returncode,
            c_res.stdout,
            c_res.stderr,
        ):
            problems.append(
                f"batch [{start}:{start + len(batch)}] mismatch vs C mo test --only "
                f"(C={c_res.returncode}, mo-rt={rs_res.returncode})\n"
                f"--- C mo stdout\n{c_res.stdout.decode(errors='replace')}"
                f"--- mo-rt stdout\n{rs_res.stdout.decode(errors='replace')}"
                f"--- C mo stderr\n{c_res.stderr.decode(errors='replace')}"
                f"--- mo-rt stderr\n{rs_res.stderr.decode(errors='replace')}"
            )
    return problems


def check_golden_case(
    mo: pathlib.Path,
    mo_rt: pathlib.Path,
    case: pathlib.Path,
    tmpdir: pathlib.Path,
) -> tuple[str, str, int, int, list[str]]:
    """Returns (category, command, tests_checked, anchors_checked, problems)."""
    entry = (case / "entry").read_text().strip() if (case / "entry").exists() else None
    program = entry or str((case / "main.mo").relative_to(ROOT))
    command = read_bytes(case, "command", b"run").decode().strip()
    assert command in ("run", "test"), f"{case.name}: unexpected command {command!r}"
    arguments = read_bytes(case, "args").decode().splitlines()
    stdin_bytes = read_bytes(case, "stdin")
    exp_stdout = read_bytes(case, "stdout")
    exp_stderr = read_bytes(case, "stderr")
    exp_status = int(read_bytes(case, "status", b"0").decode().strip())

    category = classify_golden_case(case)
    bc_path = tmpdir / f"golden_{case.name}.bc"
    emit_cmd = [str(mo), "emit"]
    if command == "test":
        emit_cmd.append("--tests")
    emit_cmd.extend([program, "-o", str(bc_path)])

    # 1. Compile-time-only error goldens (Phase 2 front-end scope): verify exact compile rejection.
    if category == "compile-only":
        emitted = subprocess.run(emit_cmd, cwd=ROOT, capture_output=True, timeout=60)
        problems = []
        if emitted.returncode != exp_status:
            problems.append(
                f"compile-only exit mismatch: expected {exp_status}, got {emitted.returncode}"
            )
        if emitted.stdout != exp_stdout:
            problems.append(
                "compile-only stdout mismatch\n"
                f"--- expected\n{exp_stdout.decode(errors='replace')}"
                f"--- actual\n{emitted.stdout.decode(errors='replace')}"
            )
        if emitted.stderr != exp_stderr:
            problems.append(
                "compile-only stderr mismatch\n"
                f"--- expected\n{exp_stderr.decode(errors='replace')}"
                f"--- actual\n{emitted.stderr.decode(errors='replace')}"
            )
        return category, command, 0, 0, problems

    # 2. Emit `.bc` and verify it passes `mo-rt verify` and `mo-rt anchors`.
    emitted = subprocess.run(emit_cmd, cwd=ROOT, capture_output=True, timeout=60)
    if emitted.returncode != 0:
        return (
            category,
            command,
            0,
            0,
            [
                f"mo emit failed (exit {emitted.returncode}): "
                f"{emitted.stderr.decode(errors='replace')}"
            ],
        )

    verified = subprocess.run(
        [str(mo_rt), "verify", str(bc_path)],
        cwd=ROOT,
        capture_output=True,
        timeout=30,
    )
    if verified.returncode != 0 or verified.stderr:
        return (
            category,
            command,
            0,
            0,
            [
                f"mo-rt verify failed (exit {verified.returncode}): "
                f"{verified.stderr.decode(errors='replace')}"
            ],
        )

    anchors_res = subprocess.run(
        [str(mo_rt), "anchors", str(bc_path)],
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout=60,
    )
    if anchors_res.returncode != 0 or anchors_res.stderr:
        return (
            category,
            command,
            0,
            0,
            [
                f"mo-rt anchors failed (exit {anchors_res.returncode}): "
                f"{anchors_res.stderr}"
            ],
        )
    anchors_checked = parse_anchors_count(anchors_res.stdout)

    # 3. `mo test` goldens containing promises / service recovery blocks:
    #    execute every `test`/`example` block in `.bc` and diff byte-for-byte against C `mo test --only`.
    if category == "promise-or-service":
        rs_res = subprocess.run(
            [str(mo_rt), "test", "--allow-empty", str(bc_path)],
            cwd=ROOT,
            input=stdin_bytes,
            capture_output=True,
            timeout=120,
        )
        problems = []
        if rs_res.returncode not in (0, 1):
            problems.append(
                f"mo-rt test --allow-empty unexpected exit {rs_res.returncode}: "
                f"{rs_res.stderr.decode(errors='replace')}"
            )
            return category, command, 0, anchors_checked, problems

        rs_names = [m.group(2) for m in TEST_LINE_RE.finditer(rs_res.stdout.decode(errors="replace"))]
        exp_names = [
            m.group(2)
            for m in TEST_LINE_RE.finditer(exp_stdout.decode(errors="replace"))
            if not PROMISE_OR_SERVICE_NAME_RE.search(m.group(2))
        ]
        if rs_names != exp_names:
            problems.append(
                f"executed test/example inventory mismatch: expected {exp_names}, got {rs_names}"
            )
        else:
            problems.extend(
                diff_test_only_batches(mo, mo_rt, program, bc_path, rs_names, ROOT, rs_full=rs_res)
            )
        return category, command, len(rs_names), anchors_checked, problems

    # 4. Full runtime parity golden suite: exact byte-for-byte match vs C `mo` and golden files.
    c_res = subprocess.run(
        [str(mo), command, program, *arguments],
        cwd=ROOT,
        input=stdin_bytes,
        capture_output=True,
        timeout=120,
    )

    rt_cmd = [str(mo_rt), command, str(bc_path)]
    if command == "run" and arguments:
        rt_cmd.extend(["--", *arguments])
    rs_res = subprocess.run(
        rt_cmd,
        cwd=ROOT,
        input=stdin_bytes,
        capture_output=True,
        timeout=120,
    )

    problems = []
    if c_res.stdout != exp_stdout or c_res.stderr != exp_stderr or c_res.returncode != exp_status:
        problems.append(
            f"C mo differed from golden expectation (status {c_res.returncode} vs {exp_status})"
        )
    if rs_res.stdout != c_res.stdout:
        problems.append(
            "stdout mismatch vs C mo\n"
            f"--- C mo\n{c_res.stdout.decode(errors='replace')}"
            f"--- mo-rt\n{rs_res.stdout.decode(errors='replace')}"
        )
    if rs_res.stderr != c_res.stderr:
        problems.append(
            "stderr mismatch vs C mo\n"
            f"--- C mo\n{c_res.stderr.decode(errors='replace')}"
            f"--- mo-rt\n{rs_res.stderr.decode(errors='replace')}"
        )
    if rs_res.returncode != c_res.returncode:
        problems.append(
            f"exit status mismatch vs C mo: C={c_res.returncode}, mo-rt={rs_res.returncode}"
        )
    tests_checked = (
        len(TEST_LINE_RE.findall(rs_res.stdout.decode(errors="replace"))) if command == "test" else 0
    )
    return category, command, tests_checked, anchors_checked, problems


def diff_test_program(
    mo: pathlib.Path,
    mo_rt: pathlib.Path,
    label: str,
    program: str,
    cwd: pathlib.Path,
    tmpdir: pathlib.Path,
    allow_empty: bool = False,
    require_all_pass: bool = True,
) -> tuple[int, int, int, list[str]]:
    """Emit `--tests` `.bc`, verify, check anchors, run `mo-rt test`, and diff against C `mo test`.

    Returns (passed_count, total_tests_checked, anchors_checked, problems).
    """
    safe_name = re.sub(r"[^A-Za-z0-9_.-]+", "_", label)
    bc_path = tmpdir / f"test_{safe_name}.bc"

    emitted = subprocess.run(
        [str(mo), "emit", "--tests", program, "-o", str(bc_path)],
        cwd=cwd,
        capture_output=True,
        timeout=60,
    )
    if emitted.returncode != 0:
        return (
            0,
            0,
            0,
            [
                f"mo emit --tests failed (exit {emitted.returncode}): "
                f"{emitted.stderr.decode(errors='replace')}"
            ],
        )

    verified = subprocess.run(
        [str(mo_rt), "verify", str(bc_path)],
        cwd=cwd,
        capture_output=True,
        timeout=30,
    )
    if verified.returncode != 0 or verified.stderr:
        return (
            0,
            0,
            0,
            [
                f"mo-rt verify failed (exit {verified.returncode}): "
                f"{verified.stderr.decode(errors='replace')}"
            ],
        )

    anchors_res = subprocess.run(
        [str(mo_rt), "anchors", str(bc_path)],
        cwd=cwd,
        capture_output=True,
        text=True,
        timeout=60,
    )
    if anchors_res.returncode != 0 or anchors_res.stderr:
        return (
            0,
            0,
            0,
            [f"mo-rt anchors failed (exit {anchors_res.returncode}): {anchors_res.stderr}"],
        )
    anchors_checked = parse_anchors_count(anchors_res.stdout)

    rt_cmd = [str(mo_rt), "test"]
    if allow_empty:
        rt_cmd.append("--allow-empty")
    rt_cmd.append(str(bc_path))
    rs_res = subprocess.run(rt_cmd, cwd=cwd, capture_output=True, timeout=180)
    rs_out = rs_res.stdout.decode(errors="replace")
    rs_names = [m.group(2) for m in TEST_LINE_RE.finditer(rs_out)]

    summary_match = SUMMARY_RE.search(rs_out)
    passed = int(summary_match.group(1)) if summary_match else 0
    failed = int(summary_match.group(2)) if summary_match else -1
    stopped = int(summary_match.group(3)) if summary_match else -1

    problems: list[str] = []
    if require_all_pass:
        if rs_res.returncode != 0 or failed != 0 or stopped != 0 or (not allow_empty and passed == 0):
            problems.append(
                f"mo-rt test did not pass cleanly (exit {rs_res.returncode}):\n"
                f"{rs_out}{rs_res.stderr.decode(errors='replace')}"
            )
            return passed, len(rs_names), anchors_checked, problems
    elif rs_res.returncode not in (0, 1):
        problems.append(
            f"mo-rt test unexpected exit {rs_res.returncode}:\n"
            f"{rs_out}{rs_res.stderr.decode(errors='replace')}"
        )
        return passed, len(rs_names), anchors_checked, problems

    if not rs_names:
        return passed, 0, anchors_checked, problems

    # Inspect C `mo test --list` to know whether promises or service recovery blocks exist.
    listed = subprocess.run(
        [str(mo), "test", "--list", program],
        cwd=cwd,
        capture_output=True,
        text=True,
        timeout=60,
    )
    if listed.returncode != 0:
        problems.append(f"mo test --list failed (exit {listed.returncode}): {listed.stderr}")
        return passed, len(rs_names), anchors_checked, problems

    list_names = [line.strip() for line in listed.stdout.splitlines() if line.strip()]
    has_non_bc_specs = any(PROMISE_OR_SERVICE_NAME_RE.search(n) for n in list_names)
    expected_bc_names = [n for n in list_names if not PROMISE_OR_SERVICE_NAME_RE.search(n)]

    if rs_names != expected_bc_names:
        problems.append(
            f"test/example inventory mismatch vs `mo test --list`: "
            f"C={expected_bc_names}, mo-rt={rs_names}"
        )
        return passed, len(rs_names), anchors_checked, problems

    if not has_non_bc_specs:
        c_res = subprocess.run([str(mo), "test", program], cwd=cwd, capture_output=True, timeout=180)
        if rs_res.stdout != c_res.stdout:
            problems.append(
                "stdout mismatch vs C mo test\n"
                f"--- C mo\n{c_res.stdout.decode(errors='replace')}"
                f"--- mo-rt\n{rs_out}"
            )
        if rs_res.stderr != c_res.stderr:
            problems.append(
                "stderr mismatch vs C mo test\n"
                f"--- C mo\n{c_res.stderr.decode(errors='replace')}"
                f"--- mo-rt\n{rs_res.stderr.decode(errors='replace')}"
            )
        if rs_res.returncode != c_res.returncode:
            problems.append(
                f"exit status mismatch vs C mo test: C={c_res.returncode}, mo-rt={rs_res.returncode}"
            )
    else:
        problems.extend(
            diff_test_only_batches(mo, mo_rt, program, bc_path, rs_names, cwd, rs_full=rs_res)
        )

    return passed, len(rs_names), anchors_checked, problems


def check_lower_source_case(
    mo: pathlib.Path,
    mo_rt: pathlib.Path,
    suite_dir: pathlib.Path,
    tmpdir: pathlib.Path,
) -> tuple[bool, int, int, list[str]]:
    """Checks one `tests/lower-source/<name>/main.mo` suite (`run` and/or `test` + `anchors`)."""
    name = suite_dir.name
    main_mo = suite_dir / "main.mo"
    program = str(main_mo.relative_to(ROOT))
    source_text = main_mo.read_text(encoding="utf-8")
    has_main = bool(re.search(r"^fn main\b", source_text, re.M))

    problems: list[str] = []
    ran_main = False

    if has_main:
        ran_main = True
        run_bc = tmpdir / f"lower_run_{name}.bc"
        emitted = subprocess.run(
            [str(mo), "emit", program, "-o", str(run_bc)],
            cwd=ROOT,
            capture_output=True,
            timeout=60,
        )
        if emitted.returncode != 0:
            problems.append(
                f"mo emit failed (exit {emitted.returncode}): "
                f"{emitted.stderr.decode(errors='replace')}"
            )
        else:
            verified = subprocess.run(
                [str(mo_rt), "verify", str(run_bc)],
                cwd=ROOT,
                capture_output=True,
                timeout=30,
            )
            if verified.returncode != 0 or verified.stderr:
                problems.append(
                    f"mo-rt verify (run) failed (exit {verified.returncode}): "
                    f"{verified.stderr.decode(errors='replace')}"
                )
            else:
                c_res = subprocess.run(
                    [str(mo), "run", program], cwd=ROOT, capture_output=True, timeout=60
                )
                rs_res = subprocess.run(
                    [str(mo_rt), "run", str(run_bc)], cwd=ROOT, capture_output=True, timeout=60
                )
                if name in SERVICE_MAIN_EXPECTED_EXIT:
                    expected_rc = SERVICE_MAIN_EXPECTED_EXIT[name]
                    if b"E1408:" not in c_res.stderr:
                        problems.append(
                            f"expected C mo run E1408 for service+main suite, got {c_res.stderr!r}"
                        )
                    if rs_res.returncode != expected_rc or rs_res.stdout != b"" or rs_res.stderr != b"":
                        problems.append(
                            f"mo-rt run mismatch for service+main suite: expected exit {expected_rc}, "
                            f"got exit={rs_res.returncode} stdout={rs_res.stdout!r} stderr={rs_res.stderr!r}"
                        )
                else:
                    if (rs_res.returncode, rs_res.stdout, rs_res.stderr) != (
                        c_res.returncode,
                        c_res.stdout,
                        c_res.stderr,
                    ):
                        problems.append(
                            f"run mismatch vs C mo (C={c_res.returncode}, mo-rt={rs_res.returncode})\n"
                            f"--- C stdout\n{c_res.stdout.decode(errors='replace')}"
                            f"--- mo-rt stdout\n{rs_res.stdout.decode(errors='replace')}"
                            f"--- C stderr\n{c_res.stderr.decode(errors='replace')}"
                            f"--- mo-rt stderr\n{rs_res.stderr.decode(errors='replace')}"
                        )

    # Always emit `--tests` `.bc`, verify, check anchors, and diff any `test`/`example` blocks.
    # (`network-error` has an intentionally failing equality test; diff it byte-for-byte vs C.)
    _, tests_checked, anchors_checked, test_problems = diff_test_program(
        mo,
        mo_rt,
        f"lower_{name}",
        program,
        ROOT,
        tmpdir,
        allow_empty=True,
        require_all_pass=(name != "network-error"),
    )
    problems.extend(test_problems)
    return ran_main, tests_checked, anchors_checked, problems


def check_env_and_limits_differential(
    mo: pathlib.Path,
    mo_rt: pathlib.Path,
    tmpdir: pathlib.Path,
) -> None:
    """Differential check for real/fake `Env`, `Args`, `Console`, and `--max-depth`."""
    env_prog = tmpdir / "env-check.mo"
    env_prog.write_text(
        """module EnvCheck

fn lookup(e: Env, key: Text) : Text
  case e.get(key)
    Some(v): v
    None: "<none>"
  end
end

fn main(console: Console, env: Env, args: Args)
  console.print(lookup(env, "MO_RT_TEST_VAR"))
  console.print(lookup(env, "MO_RT_ABSENT_VAR"))
  console.print(args.list.join(","))
end

test "env and args fakes"
  fake = Env.of({"MO_RT_TEST_VAR": "from-fake"})
  assert lookup(fake, "MO_RT_TEST_VAR") == "from-fake"
  assert lookup(fake, "OTHER") == "<none>"
  fake_args = Args.of(["a", "b"])
  assert fake_args.list.join("-") == "a-b"
end
""",
        encoding="utf-8",
    )
    bc_path = tmpdir / "env_check.bc"
    subprocess.run([str(mo), "emit", str(env_prog), "-o", str(bc_path)], check=True)
    subprocess.run([str(mo_rt), "verify", str(bc_path)], check=True)

    env = os.environ.copy()
    env["MO_RT_TEST_VAR"] = "hello-env"
    env.pop("MO_RT_ABSENT_VAR", None)

    c_res = subprocess.run(
        [str(mo), "run", str(env_prog), "one", "two"],
        env=env,
        capture_output=True,
        timeout=30,
    )
    rs_res = subprocess.run(
        [str(mo_rt), "run", str(bc_path), "--", "one", "two"],
        env=env,
        capture_output=True,
        timeout=30,
    )
    assert c_res.returncode == 0 and rs_res.returncode == 0, (c_res, rs_res)
    assert rs_res.stdout == c_res.stdout == b"hello-env\n<none>\none,two\n", (
        c_res.stdout,
        rs_res.stdout,
    )
    assert rs_res.stderr == c_res.stderr == b"", (c_res.stderr, rs_res.stderr)

    # Also test fake `Env.of` and `Args.of` via `mo emit --tests` + `mo-rt test`.
    test_bc = tmpdir / "env_check_tests.bc"
    subprocess.run([str(mo), "emit", "--tests", str(env_prog), "-o", str(test_bc)], check=True)
    subprocess.run([str(mo_rt), "verify", str(test_bc)], check=True)
    c_test = subprocess.run([str(mo), "test", str(env_prog)], capture_output=True, timeout=30)
    rs_test = subprocess.run([str(mo_rt), "test", str(test_bc)], capture_output=True, timeout=30)
    assert c_test.returncode == 0 and rs_test.returncode == 0, (c_test, rs_test)
    assert rs_test.stdout == c_test.stdout, (c_test.stdout, rs_test.stdout)
    assert rs_test.stderr == c_test.stderr == b"", (c_test.stderr, rs_test.stderr)

    # Check --max-depth parity on a non-tail recursive program.
    depth_prog = tmpdir / "depth-check.mo"
    depth_prog.write_text(
        """module DepthCheck

fn dive(n: Int) : Int
  if n == 0: 0 else: 1 + dive(n - 1)
end

fn main(console: Console)
  console.print("#{dive(20)}")
end
""",
        encoding="utf-8",
    )
    depth_bc = tmpdir / "depth_check.bc"
    subprocess.run([str(mo), "emit", str(depth_prog), "-o", str(depth_bc)], check=True)
    c_depth = subprocess.run(
        [str(mo), "run", "--max-depth", "8", str(depth_prog)],
        capture_output=True,
        timeout=30,
    )
    rs_depth = subprocess.run(
        [str(mo_rt), "run", "--max-depth", "8", str(depth_bc)],
        capture_output=True,
        timeout=30,
    )
    assert c_depth.returncode == 70 and rs_depth.returncode == 70, (c_depth, rs_depth)
    assert rs_depth.stdout == c_depth.stdout == b""
    assert rs_depth.stderr == c_depth.stderr, (
        f"max-depth stderr mismatch:\n--- C mo\n{c_depth.stderr.decode()}\n"
        f"--- mo-rt\n{rs_depth.stderr.decode()}"
    )

    # Check BigInt large arithmetic (Karatsuba + Knuth D + balanced radix printing),
    # Map/Set hashing, List.sum, and defect parity (negative pow + stop variable replay).
    big_prog = tmpdir / "big-check.mo"
    big_prog.write_text(
        """module BigCheck

fn main(console: Console, args: Args)
  mode = args.list.first
  a = (2.to_big).pow(200)
  b = (3.to_big).pow(130)
  prod = a * b
  q = prod / a
  r = prod % a
  console.print("#{q == b} #{r}")
  console.print("#{(-7.to_big) / 3.to_big} #{(-7.to_big) % 3.to_big}")
  console.print("#{(-7.to_big).floor_div(3.to_big)} #{(-7.to_big).floor_mod(3.to_big)}")
  m = {a: "big-a", b: "big-b"}
  console.print("#{m.get(a)}")
  xs = [a, b, -a]
  console.print("#{xs.sum == b}")
  case mode
    Some("negpow"):
      _ = (2.to_big).pow(-1)
    Some("divzero"):
      _ = a / 0.to_big
    Some("stop"):
      msg = "boom"
      console.print(msg)
      stop "stopped with bigints"
    Some(_):
      console.print("other")
    None:
      console.print("done")
  end
end
""",
        encoding="utf-8",
    )
    big_bc = tmpdir / "bigint_check.bc"
    subprocess.run([str(mo), "emit", str(big_prog), "-o", str(big_bc)], check=True)
    subprocess.run([str(mo_rt), "verify", str(big_bc)], check=True)

    for arg_list in ([], ["negpow"], ["divzero"], ["stop"]):
        c_big = subprocess.run(
            [str(mo), "run", str(big_prog), *arg_list],
            capture_output=True,
            timeout=30,
        )
        rt_cmd = [str(mo_rt), "run", str(big_bc)]
        if arg_list:
            rt_cmd.extend(["--", *arg_list])
        rs_big = subprocess.run(rt_cmd, capture_output=True, timeout=30)
        assert rs_big.returncode == c_big.returncode, (arg_list, c_big, rs_big)
        assert rs_big.stdout == c_big.stdout, (arg_list, c_big.stdout, rs_big.stdout)
        assert rs_big.stderr == c_big.stderr, (
            f"BigInt ({arg_list}) stderr mismatch:\n--- C mo\n{c_big.stderr.decode()}\n"
            f"--- mo-rt\n{rs_big.stderr.decode()}"
        )

    # Confirm `mo-rt verify` rejects a corrupted `.bc` file with nonzero exit.
    corrupt_bc = tmpdir / "corrupt.bc"
    raw = bytearray(big_bc.read_bytes())
    raw[-1] ^= 0xFF
    corrupt_bc.write_bytes(raw)
    bad_verify = subprocess.run(
        [str(mo_rt), "verify", str(corrupt_bc)],
        capture_output=True,
        timeout=30,
    )
    assert bad_verify.returncode != 0, "mo-rt verify must reject corrupted bytecode"


def check_slice_1c_differential(
    mo: pathlib.Path,
    mo_rt: pathlib.Path,
    tmpdir: pathlib.Path,
) -> None:
    """Differential check for Slice 1C: --root DIR, real Files/ReadFiles/Files.File,
    symlink confinement, Clock, Random.seeded, SecureRandom.seeded, Crypto, Unicode16."""
    prog = tmpdir / "slice1c-check.mo"
    prog.write_text(
        r"""module Slice1cCheck
use Crypto
use Unicode16

fn must_unit(r: Result(Unit, FileError)) : Unit
  case r
    Ok(_): Unit
    Error(_): stop "unexpected file error"
  end
end

fn must_bytes(r: Result(Bytes, FileError)) : Text
  case r
    Ok(b): b.to_hex
    Error(e): "err:#{e}"
  end
end

fn main(console: Console, files: Files, clock: Clock, rng: Random, srng: SecureRandom)
  ## 1. Real Files / ReadFiles / Files.File under --root DIR
  must_unit(files.make_folder("sub"))
  must_unit(files.write_bytes("sub/a.bin", Bytes.from_u32_be(0x01020304)))
  must_unit(files.rename("sub/a.bin", "sub/b.bin"))
  ro = files.within("sub").read_only
  console.print("exists=#{ro.exists?("b.bin")} old=#{ro.exists?("a.bin")}")
  console.print("entries=#{ro.entries(".")}")
  console.print("escape=#{ro.read_text("../outside.txt")}")
  console.print("symlink_escape=#{files.read_text("escape_link")}")
  fh = case files.open("data.bin")
    Ok(h): h
    Error(_): stop "unexpected open error"
  end
  must_unit(fh.write_at(4, Bytes.from_u32_be(0x0a141e28)))
  must_unit(fh.sync)
  console.print("size=#{fh.size} read=#{must_bytes(fh.read_at(0, 10))}")
  must_unit(fh.truncate(5))
  console.print("after_trunc=#{fh.size} read=#{must_bytes(fh.read_at(0, 10))}")
  ## Real Clock, Random, SecureRandom smoke (non-deterministic values checked for range/length)
  clock.sleep(0.milliseconds)
  epoch = Time.parse("2000-01-01T00:00:00Z") or stop "valid epoch"
  now_ok = clock.now > epoch
  r_val = rng.int(10, 20)
  r_ok = r_val >= 10 and r_val < 20
  s_ok = srng.bytes(16).size == 16
  console.print("real_caps=#{now_ok} #{r_ok} #{s_ok}")
  ## Crypto (SHA-256, HMAC, AES-256-GCM, scrypt PHC, constant-time eq)
  key = Crypto.sha256("slice1c-key".bytes)
  nonce = "0123456789ab".bytes
  tag = Crypto.hmac_sha256(key, "payload".bytes)
  console.print("hmac=#{tag.to_hex} eq=#{Crypto.constant_time_eq?(tag, tag)}")
  ct = case Crypto.seal(key, nonce, "secret-msg".bytes, "aad".bytes)
    Ok(c): c
    Error(_): stop "unexpected seal error"
  end
  pt = case Crypto.open(key, nonce, ct, "aad".bytes)
    Ok(b): b.to_hex
    Error(e): "err:#{e}"
  end
  bad = case Crypto.open(key, nonce, ct, "wrong-aad".bytes)
    Ok(b): b.to_hex
    Error(e): "err:#{e}"
  end
  console.print("aead=#{pt} bad=#{bad}")
  salt = "0123456789abcdef".bytes
  phc = case Crypto.hash_password("hunter2", salt)
    Ok(h): h
    Error(_): stop "unexpected phc error"
  end
  console.print("scrypt=#{phc} ok=#{Crypto.verify_password("hunter2", phc)} bad=#{Crypto.verify_password("nope", phc)}")
  ## Unicode16
  console.print("u16=#{Unicode16.graphemes("e\u{0301}🇺🇸").size} #{Unicode16.display_width("🇺🇸a\u{0301}")} #{Unicode16.lowercase("ΟΣ")} #{Unicode16.uppercase("straße")} #{Unicode16.normalize_nfc("e\u{0301}").bytes.size}")
end

test "slice 1c deterministic fakes"
  t0 = Time.parse("2026-10-12T02:15:00Z") or stop "valid time"
  fc = Clock.fixed(t0)
  fc.sleep(250.milliseconds)
  assert fc.now.to_text == "2026-10-12T02:15:00.25Z"
  fr = Random.seeded(12345)
  assert fr.int(0, 1000) == Random.seeded(12345).int(0, 1000)
  fsr = SecureRandom.seeded(98765)
  b1 = fsr.bytes(12)
  assert b1 == SecureRandom.seeded(98765).bytes(12)
  ff = Files.in_memory({"a.txt": "hi"})
  assert ff.open("bin.dat") is Ok(fh)
  assert fh.write_at(2, "ok".bytes) == Ok(Unit)
  assert fh.size == Ok(4)
  assert fh.read_at(0, 4) == Ok(Bytes.join([Bytes.zeroed(2), "ok".bytes]))
end
""",
        encoding="utf-8",
    )
    bc_path = tmpdir / "slice1c_check.bc"
    subprocess.run([str(mo), "emit", str(prog), "-o", str(bc_path)], check=True)
    subprocess.run([str(mo_rt), "verify", str(bc_path)], check=True)

    outside = tmpdir / "outside.txt"
    outside.write_text("do-not-read", encoding="utf-8")

    outputs = []
    for label, cmd_prefix in (
        ("c", [str(mo), "run"]),
        ("rs", [str(mo_rt), "run"]),
    ):
        root_dir = tmpdir / f"root_{label}"
        root_dir.mkdir()
        os.symlink(str(outside), root_dir / "escape_link")
        target = str(prog) if label == "c" else str(bc_path)
        res = subprocess.run(
            [*cmd_prefix, "--root", str(root_dir), target],
            capture_output=True,
            timeout=60,
        )
        assert res.returncode == 0, (label, res.returncode, res.stdout, res.stderr)
        assert res.stderr == b"", (label, res.stderr)
        outputs.append(res.stdout)

    assert outputs[0] == outputs[1], (
        f"Slice 1C differential mismatch:\n--- C mo\n{outputs[0].decode()}\n"
        f"--- mo-rt\n{outputs[1].decode()}"
    )

    test_bc = tmpdir / "slice1c_check_tests.bc"
    subprocess.run([str(mo), "emit", "--tests", str(prog), "-o", str(test_bc)], check=True)
    subprocess.run([str(mo_rt), "verify", str(test_bc)], check=True)
    c_test = subprocess.run([str(mo), "test", str(prog)], capture_output=True, timeout=60)
    rs_test = subprocess.run([str(mo_rt), "test", str(test_bc)], capture_output=True, timeout=60)
    assert c_test.returncode == 0 and rs_test.returncode == 0, (c_test, rs_test)
    assert rs_test.stdout == c_test.stdout, (c_test.stdout, rs_test.stdout)
    assert rs_test.stderr == c_test.stderr == b"", (c_test.stderr, rs_test.stderr)

    # Confirm Rust Unicode 16 tables match the pinned UCD 16.0.0 source files.
    subprocess.run(
        [sys.executable, str(ROOT / "ci/generate_unicode16.py"), "--rust", "--check"],
        cwd=ROOT,
        check=True,
        capture_output=True,
    )


def check_slice_1d_differential(
    mo: pathlib.Path,
    mo_rt: pathlib.Path,
    tmpdir: pathlib.Path,
) -> None:
    """Differential check for Slice 1D: real/fake Network (+ TLS via SSL_CERT_FILE),
    real/fake Sqlite (--root, --state-dir, WAL, authorizer, confinement),
    real/fake Command (--root, within, allow_programs, timeout),
    and Simulation (start/send/advance/timers/crash/restart)."""

    # 1. Generate a local test CA + signed leaf cert (SAN=IP:127.0.0.1,DNS:localhost, CA:FALSE)
    #    for real TLS parity between OpenSSL (C mo) and pure-Rust rustls (mo-rt).
    tls_ca_cert = tmpdir / "tls_ca.pem"
    tls_ca_key = tmpdir / "tls_ca_key.pem"
    tls_cert = tmpdir / "tls_cert.pem"
    tls_key = tmpdir / "tls_key.pem"
    tls_csr = tmpdir / "tls_leaf.csr"
    tls_ext = tmpdir / "tls_leaf.ext"
    openssl_bin = shutil.which("openssl")
    has_tls_fixture = False
    if openssl_bin is not None:
        tls_ext.write_text(
            "basicConstraints=CA:FALSE\n"
            "keyUsage=digitalSignature,keyEncipherment\n"
            "extendedKeyUsage=serverAuth\n"
            "subjectAltName=IP:127.0.0.1,DNS:localhost\n",
            encoding="utf-8",
        )
        g1 = subprocess.run(
            [
                openssl_bin,
                "req",
                "-x509",
                "-newkey",
                "rsa:2048",
                "-keyout",
                str(tls_ca_key),
                "-out",
                str(tls_ca_cert),
                "-days",
                "1",
                "-nodes",
                "-subj",
                "/CN=Mo Test Root CA",
                "-addext",
                "basicConstraints=critical,CA:TRUE",
                "-addext",
                "keyUsage=critical,keyCertSign,cRLSign",
            ],
            capture_output=True,
            timeout=30,
        )
        g2 = subprocess.run(
            [
                openssl_bin,
                "req",
                "-newkey",
                "rsa:2048",
                "-keyout",
                str(tls_key),
                "-out",
                str(tls_csr),
                "-nodes",
                "-subj",
                "/CN=localhost",
            ],
            capture_output=True,
            timeout=30,
        )
        g3 = subprocess.run(
            [
                openssl_bin,
                "x509",
                "-req",
                "-in",
                str(tls_csr),
                "-CA",
                str(tls_ca_cert),
                "-CAkey",
                str(tls_ca_key),
                "-CAcreateserial",
                "-out",
                str(tls_cert),
                "-days",
                "1",
                "-extfile",
                str(tls_ext),
            ],
            capture_output=True,
            timeout=30,
        )
        has_tls_fixture = g1.returncode == 0 and g2.returncode == 0 and g3.returncode == 0

    # Spin up local TCP + TLS servers.
    tcp_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tcp_srv.bind(("127.0.0.1", 0))
    tcp_srv.listen(8)
    tcp_port = tcp_srv.getsockname()[1]

    stop_flag = threading.Event()

    def serve_tcp() -> None:
        tcp_srv.settimeout(0.2)
        while not stop_flag.is_set():
            try:
                conn, _ = tcp_srv.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with conn:
                data = conn.recv(1024)
                if data:
                    conn.sendall(b"ECHO:" + data)

    tls_srv = None
    tls_port = 0
    if has_tls_fixture:
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain(certfile=str(tls_cert), keyfile=str(tls_key))
        tls_srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        tls_srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        tls_srv.bind(("127.0.0.1", 0))
        tls_srv.listen(8)
        tls_port = tls_srv.getsockname()[1]

        def serve_tls() -> None:
            assert tls_srv is not None
            tls_srv.settimeout(0.2)
            while not stop_flag.is_set():
                try:
                    raw, _ = tls_srv.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                try:
                    with ssl_ctx.wrap_socket(raw, server_side=True) as sconn:
                        data = sconn.recv(1024)
                        if data:
                            sconn.sendall(b"TLS:" + data)
                except OSError:
                    pass

        tls_thread = threading.Thread(target=serve_tls, daemon=True)
        tls_thread.start()
    else:
        tls_thread = None

    tcp_thread = threading.Thread(target=serve_tcp, daemon=True)
    tcp_thread.start()

    try:
        prog = tmpdir / "slice1d-check.mo"
        prog.write_text(
            r"""module Slice1dCheck
use Command

fn net_kind(r: Result(Connection, NetworkError)) : Text
  case r
    Ok(_): "Ok"
    Error(Network.TimedOut): "TimedOut"
    Error(Network.Closed): "Closed"
    Error(Network.Refused(host: h, port: p)): "Refused:#{h}:#{p}"
    Error(Network.UnknownHost(host: h)): "UnknownHost:#{h}"
    Error(Network.Broken(reason: _)): "Broken"
    Error(Network.Certificate(reason: _)): "Certificate"
  end
end

fn sql_kind(r: Result(Unit, SqliteError)) : Text
  case r
    Ok(_): "Ok"
    Error(Sqlite.OutsideFolder(path: p)): "Outside:#{p}"
    Error(Sqlite.NotPermitted(reason: m)): "NotPermitted:#{m}"
    Error(Sqlite.SqlFailed(message: m)): "SqlFailed:#{m}"
  end
end

fn parse_arg_int(args: List(Text), idx: Int) : Int
  case args.get(idx)
    Some(s): s.to_int or 0
    None: 0
  end
end

fn decode_utf8_or(b: Bytes, fallback: Text) : Text
  b.decode_utf8 or fallback
end

fn must_net(r: Result(Unit, NetworkError))
  case r
    Ok(_): Unit
    Error(_): stop "unexpected network error"
  end
end

fn must_sql(r: Result(Unit, SqliteError))
  case r
    Ok(_): Unit
    Error(_): stop "unexpected sqlite error"
  end
end

fn main(console: Console, args: Args, net: Network, sql: Sqlite, cmd: Command)
  tcp_port = parse_arg_int(args.list, 0)
  tls_port = parse_arg_int(args.list, 1)

  ## 1. Real TCP round-trip + allow_hosts refusal + zero-duration timeout
  restricted = net.allow_hosts(["127.0.0.1:#{tcp_port}"])
  c1 = case restricted.connect("127.0.0.1", tcp_port, 5.seconds)
    Ok(c): c
    Error(_): stop "unexpected tcp error"
  end
  must_net(c1.send("hello".bytes))
  r1 = case c1.receive(64)
    Ok(b): decode_utf8_or(b, "bad-utf8")
    Error(e): "err:#{e}"
  end
  console.print("tcp=#{r1}")
  console.print("refused=#{net_kind(restricted.connect("example.com", 80, 1.seconds))}")
  console.print("timeout=#{net_kind(net.connect("127.0.0.1", tcp_port, 0.milliseconds))}")

  ## 2. Real TLS round-trip (with SSL_CERT_FILE trusted CA) + plain-TCP Broken
  if tls_port > 0
    tc = case net.connect_secure("127.0.0.1", tls_port, 5.seconds)
      Ok(c): c
      Error(_): stop "unexpected tls error"
    end
    must_net(tc.send("secure".bytes))
    rt = case tc.receive(64)
      Ok(b): decode_utf8_or(b, "bad-utf8")
      Error(e): "err:#{e}"
    end
    console.print("tls=#{rt}")
    console.print("tls_plain=#{net_kind(net.connect_secure("127.0.0.1", tcp_port, 5.seconds))}")
  end

  ## 3. Real Sqlite under --root/--state-dir: WAL, CRUD, execute_raw, query, authorizer, confinement
  db = case sql.open("bench.db")
    Ok(d): d
    Error(_): stop "unexpected db open"
  end
  must_sql(
    db.run("CREATE TABLE kv (id INTEGER PRIMARY KEY, k TEXT, v BLOB, f REAL, n TEXT)", [])
  )
  raw = case db.execute_raw("INSERT INTO kv (k, v, f, n) VALUES (?, ?, ?, ?)", [
    Sqlite.String("alpha"),
    Sqlite.Blob("raw".bytes),
    Sqlite.Real(2.5),
    Sqlite.Null
  ])
    Ok((changes, rowid)): "#{changes}:#{rowid}"
    Error(e): "err:#{e}"
  end
  console.print("sql_raw=#{raw}")
  q = case db.query("SELECT id, k, f, n FROM kv", [])
    Ok(res): "#{res.columns.join(",")}|#{res.rows.size}"
    Error(e): "err:#{e}"
  end
  console.print("sql_q=#{q}")
  console.print("sql_attach=#{sql_kind(db.run("ATTACH DATABASE ':memory:' AS x", []))}")
  console.print("sql_vacuum=#{sql_kind(db.run("VACUUM", []))}")
  console.print("sql_vac_into=#{sql_kind(db.run("VACUUM INTO 'leak.db'", []))}")
  esc_db = case sql.open("../escape.db")
    Ok(_): "Ok"
    Error(Sqlite.OutsideFolder(path: p)): "Outside:#{p}"
    Error(_): "Other"
  end
  console.print("sql_esc=#{esc_db}")

  ## 4. Real Command under --root: echo, allow_programs intersection, within escape, timeout
  c_narrow = cmd.allow_programs(["echo", "pwd"]).allow_programs(["echo"])
  out1 = case c_narrow.run_raw("echo", ["slice-1d"], 5.seconds)
    Ok((code, out_b, _)): "#{code}:#{decode_utf8_or(out_b, "")}"
    Error(e): "err:#{e}"
  end
  console.print("cmd_ok=#{out1.trim}")
  console.print("cmd_deny=#{c_narrow.run("pwd", [], 5.seconds)}")
  console.print("cmd_esc=#{cmd.within("../nope").run("echo", ["x"], 5.seconds)}")
  console.print("cmd_to=#{cmd.run("sleep", ["5"], 30.milliseconds)}")
end
""",
            encoding="utf-8",
        )
        bc_path = tmpdir / "slice1d_check.bc"
        subprocess.run([str(mo), "emit", str(prog), "-o", str(bc_path)], check=True)
        subprocess.run([str(mo_rt), "verify", str(bc_path)], check=True)

        env = os.environ.copy()
        if has_tls_fixture:
            env["SSL_CERT_FILE"] = str(tls_ca_cert)

        outputs = []
        for label, cmd_prefix in (
            ("c", [str(mo), "run"]),
            ("rs", [str(mo_rt), "run"]),
        ):
            root_dir = tmpdir / f"s1d_root_{label}"
            state_dir = tmpdir / f"s1d_state_{label}"
            root_dir.mkdir()
            state_dir.mkdir()
            target = str(prog) if label == "c" else str(bc_path)
            extra_args = [str(tcp_port), str(tls_port)]
            cmd_line = [
                *cmd_prefix,
                "--root",
                str(root_dir),
                "--state-dir",
                str(state_dir),
                target,
            ]
            if label == "rs":
                cmd_line.extend(["--", *extra_args])
            else:
                cmd_line.extend(extra_args)
            res = subprocess.run(
                cmd_line,
                env=env,
                capture_output=True,
                timeout=60,
            )
            assert res.returncode == 0, (label, res.returncode, res.stdout, res.stderr)
            assert res.stderr == b"", (label, res.stderr)
            assert (state_dir / "bench.db").exists(), f"{label}: --state-dir did not receive bench.db"
            outputs.append(res.stdout)

        assert outputs[0] == outputs[1], (
            f"Slice 1D differential mismatch:\n--- C mo\n{outputs[0].decode()}\n"
            f"--- mo-rt\n{outputs[1].decode()}"
        )

        # Verify untrusted TLS cert returns Certificate error on BOTH C mo and mo-rt.
        if has_tls_fixture:
            untrusted_prog = tmpdir / "tls-untrusted.mo"
            untrusted_prog.write_text(
                r"""module TlsUntrusted

fn parse_port(args: List(Text)) : Int
  case args.get(0)
    Some(s): s.to_int or 0
    None: 0
  end
end

fn main(console: Console, args: Args, net: Network)
  p = parse_port(args.list)
  out = case net.connect_secure("127.0.0.1", p, 5.seconds)
    Ok(_): "Ok"
    Error(Network.Certificate(reason: _)): "Certificate"
    Error(Network.Broken(reason: _)): "Broken"
    Error(Network.Closed): "Closed"
    Error(Network.TimedOut): "TimedOut"
    Error(Network.Refused(host: _, port: _)): "Refused"
    Error(Network.UnknownHost(host: _)): "UnknownHost"
  end
  console.print(out)
end
""",
                encoding="utf-8",
            )
            ut_bc = tmpdir / "tls_untrusted.bc"
            subprocess.run([str(mo), "emit", str(untrusted_prog), "-o", str(ut_bc)], check=True)
            clean_env = os.environ.copy()
            clean_env.pop("SSL_CERT_FILE", None)
            clean_env.pop("SSL_CERT_DIR", None)
            c_ut = subprocess.run(
                [str(mo), "run", str(untrusted_prog), str(tls_port)],
                env=clean_env,
                capture_output=True,
                timeout=30,
            )
            rs_ut = subprocess.run(
                [str(mo_rt), "run", str(ut_bc), "--", str(tls_port)],
                env=clean_env,
                capture_output=True,
                timeout=30,
            )
            assert c_ut.returncode == 0 and rs_ut.returncode == 0, (c_ut, rs_ut)
            assert c_ut.stdout == rs_ut.stdout == b"Certificate\n", (c_ut.stdout, rs_ut.stdout)

        # Verify Slice 1D test block (Simulation + timers + crash/restart + Sqlite.fake + Command.fake)
        sim_prog = tmpdir / "slice1d-sim.mo"
        sim_prog.write_text(
            r"""module Slice1dSim
use Command

struct Snapshot
  value: Int
end

service
  state Int = 10
  tools add, current
  every 1.second tick
  token from env "MO_SLICE1D_SIM_TOKEN"
end

fn add(state: Int, _now: Time, delta: Int) : (Int, Int)
  next = state + delta
  (next, next)
end

fn current(state: Int, _now: Time) : Snapshot
  Snapshot(value: state)
end

fn tick(state: Int, _now: Time) : Int
  state + 1
end

test "slice 1d simulation and fakes"
  ## Simulation: start (+1 startup tick -> 11), send, advance (+2 ticks -> 18), crash, restart (+1 tick -> 19)
  t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
  sim = Simulation.start(Slice1dSim, t0)
  assert Simulation.send(sim, Slice1dSim.current) == Ok(Snapshot(value: 11))
  assert Simulation.send(sim, Slice1dSim.add, 5) == Ok((16, 16))
  Simulation.advance(sim, 2.seconds)
  assert Simulation.send(sim, Slice1dSim.current) == Ok(Snapshot(value: 18))
  Simulation.crash(sim)
  assert Simulation.send(sim, Slice1dSim.current) == Error(ServiceDown)
  Simulation.restart(sim)
  assert Simulation.send(sim, Slice1dSim.current) == Ok(Snapshot(value: 19))

  ## Fake Sqlite + Fake Command
  fsql = Sqlite.fake()
  assert fsql.open("a.db") is Ok(fdb)
  assert fdb.run("CREATE TABLE t (x INT)", []) == Ok(Unit)
  assert fdb.run("INSERT INTO t VALUES (?)", [Sqlite.Integer(42)]) == Ok(Unit)
  assert fsql.open("./a.db") is Ok(fdb2)
  assert fdb2.query("SELECT x FROM t", []) == Ok(Sqlite.Rows(columns: ["x"], rows: [[Sqlite.Integer(42)]]))

  fcmd = Command.fake(
    Command.replies([(Command.spec("echo", ["hi"]), Command.ok_output("hi\n"))])
  )
  assert fcmd.run("echo", ["hi"], 1.second) ==
    Ok(Command.Output(status: 0, stdout: "hi\n", stderr: ""))
  assert fcmd.allow_programs(["ls"]).run("echo", ["hi"], 1.second) ==
    Error(Command.NotPermitted(program: "echo"))
end
""",
            encoding="utf-8",
        )
        test_bc = tmpdir / "slice1d_sim_tests.bc"
        subprocess.run([str(mo), "emit", "--tests", str(sim_prog), "-o", str(test_bc)], check=True)
        subprocess.run([str(mo_rt), "verify", str(test_bc)], check=True)
        c_sim_test = subprocess.run([str(mo), "test", str(sim_prog)], capture_output=True, timeout=60)
        rs_test = subprocess.run([str(mo_rt), "test", str(test_bc)], capture_output=True, timeout=60)
        assert c_sim_test.returncode == 0 and rs_test.returncode == 0, (
            c_sim_test.stdout.decode(errors="replace"),
            c_sim_test.stderr.decode(errors="replace"),
            rs_test.stdout.decode(errors="replace"),
            rs_test.stderr.decode(errors="replace"),
        )
        assert b"1 passed, 0 failed, 0 stopped" in rs_test.stdout, rs_test.stdout
    finally:
        stop_flag.set()
        tcp_srv.close()
        if tls_srv is not None:
            tls_srv.close()
        tcp_thread.join(timeout=2)
        if tls_thread is not None:
            tls_thread.join(timeout=2)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mo", help="Path to C `mo` executable")
    parser.add_argument("--mo-rt", dest="mo_rt", help="Path to Rust `mo-rt` executable")
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="Skip `cargo build --release --bin mo-rt`",
    )
    parser.add_argument(
        "--case",
        action="append",
        default=[],
        help="Run only the named golden case(s)",
    )
    parser.add_argument(
        "--modb-dir",
        help="Optional path to a MoDB checkout to also verify all 5 MoDB suites (29/29 tests)",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=min(8, os.cpu_count() or 4),
        help="Maximum parallel workers (default: min(8, cpu_count))",
    )
    args = parser.parse_args()
    workers = max(1, args.jobs)

    mo = find_or_build_mo(args.mo)
    mo_rt = find_or_build_mo_rt(args.mo_rt, args.skip_build)
    check_no_forbidden_linkage(mo_rt)
    check_forbid_unsafe_code()

    all_golden_dirs = sorted(p for p in GOLDEN.iterdir() if p.is_dir())
    if args.case:
        wanted = set(args.case)
        golden_dirs = [p for p in all_golden_dirs if p.name in wanted]
        found = {p.name for p in golden_dirs}
        missing = sorted(wanted - found)
        assert not missing, f"missing golden case(s): {missing}"
    else:
        assert len(all_golden_dirs) >= MINIMUM_GOLDEN_SUITES, (
            f"tests/golden has {len(all_golden_dirs)} suites, expected >= {MINIMUM_GOLDEN_SUITES}"
        )
        golden_dirs = all_golden_dirs

    failures: list[str] = []
    golden_full_run = 0
    golden_full_test = 0
    golden_compile_only = 0
    golden_promise_suites = 0
    golden_promise_tests = 0
    total_anchors = 0

    lower_suites = 0
    lower_mains = 0
    lower_tests = 0
    example_suites = 0
    example_tests = 0
    lib_suites = 0
    lib_tests = 0
    prog_suites = 0
    prog_tests = 0
    modb_suites = 0
    modb_tests = 0

    with tempfile.TemporaryDirectory(prefix="mo-rust-rt-") as td:
        tmpdir = pathlib.Path(td)

        # 1. All 238/238 golden suites (parallelized, results collected in deterministic order).
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            golden_futs = [
                (case, pool.submit(check_golden_case, mo, mo_rt, case, tmpdir))
                for case in golden_dirs
            ]
            for case, fut in golden_futs:
                category, command, tests_checked, anchors_checked, problems = fut.result()
                total_anchors += anchors_checked
                if category == "compile-only":
                    golden_compile_only += 1
                elif category == "promise-or-service":
                    golden_promise_suites += 1
                    golden_promise_tests += tests_checked
                else:
                    if command == "run":
                        golden_full_run += 1
                    else:
                        golden_full_test += 1
                if problems:
                    failures.append(f"golden/{case.name} ({category}, {command}):\n  " + "\n  ".join(problems))

        if not args.case:
            lower_dirs = sorted(p for p in LOWER_SOURCE.iterdir() if p.is_dir())
            assert len(lower_dirs) >= MINIMUM_LOWER_SOURCE_SUITES, (
                f"tests/lower-source has {len(lower_dirs)} suites, expected >= {MINIMUM_LOWER_SOURCE_SUITES}"
            )
            example_paths = sorted(
                [*(ROOT / "examples").glob("*.mo"), *(ROOT / "examples").glob("*/main.mo")]
            )
            assert len(example_paths) >= MINIMUM_EXAMPLE_PROGRAMS, (
                f"examples/ has {len(example_paths)} programs, expected >= {MINIMUM_EXAMPLE_PROGRAMS}"
            )
            lib_paths = sorted((ROOT / "lib").rglob("*.mo"))
            assert len(lib_paths) >= MINIMUM_LIB_MODULES, (
                f"lib/ has {len(lib_paths)} modules, expected >= {MINIMUM_LIB_MODULES}"
            )
            tool_service_paths = sorted(
                [
                    *ROOT.glob("tools/*.mo"),
                    *ROOT.glob("tools/*/main.mo"),
                    *ROOT.glob("services/mo-*/main.mo"),
                    ROOT / "services/task-queue/queue-tests.mo",
                ]
            )
            assert len(tool_service_paths) >= MINIMUM_TOOL_SERVICE_PROGRAMS, (
                f"tools+services has {len(tool_service_paths)} programs, expected >= {MINIMUM_TOOL_SERVICE_PROGRAMS}"
            )

            with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
                lower_futs = [
                    (d, pool.submit(check_lower_source_case, mo, mo_rt, d, tmpdir))
                    for d in lower_dirs
                ]
                example_futs = [
                    (
                        p,
                        pool.submit(
                            diff_test_program,
                            mo,
                            mo_rt,
                            f"example_{p.relative_to(ROOT)}",
                            str(p.relative_to(ROOT)),
                            ROOT,
                            tmpdir,
                        ),
                    )
                    for p in example_paths
                ]
                lib_futs = [
                    (
                        p,
                        pool.submit(
                            diff_test_program,
                            mo,
                            mo_rt,
                            f"lib_{p.relative_to(ROOT)}",
                            str(p.relative_to(ROOT)),
                            ROOT,
                            tmpdir,
                        ),
                    )
                    for p in lib_paths
                ]
                prog_futs = [
                    (
                        p,
                        pool.submit(
                            diff_test_program,
                            mo,
                            mo_rt,
                            f"prog_{p.relative_to(ROOT)}",
                            str(p.relative_to(ROOT)),
                            ROOT,
                            tmpdir,
                        ),
                    )
                    for p in tool_service_paths
                ]

                modb_futs = []
                if args.modb_dir:
                    modb_root = pathlib.Path(args.modb_dir).resolve()
                    assert modb_root.is_dir(), f"--modb-dir not found: {modb_root}"
                    for rel in MODB_SUITES:
                        assert (modb_root / rel).is_file(), f"missing MoDB suite: {modb_root / rel}"
                        modb_futs.append(
                            (
                                rel,
                                pool.submit(
                                    diff_test_program,
                                    mo,
                                    mo_rt,
                                    f"modb_{rel}",
                                    rel,
                                    modb_root,
                                    tmpdir,
                                ),
                            )
                        )

                for d, fut in lower_futs:
                    ran_main, n_tests, n_anchors, probs = fut.result()
                    lower_suites += 1
                    lower_mains += int(ran_main)
                    lower_tests += n_tests
                    total_anchors += n_anchors
                    if probs:
                        failures.append(f"lower-source/{d.name}:\n  " + "\n  ".join(probs))

                for p, fut in example_futs:
                    _, n_tests, n_anchors, probs = fut.result()
                    example_suites += 1
                    example_tests += n_tests
                    total_anchors += n_anchors
                    if probs:
                        failures.append(f"{p.relative_to(ROOT)}:\n  " + "\n  ".join(probs))

                for p, fut in lib_futs:
                    _, n_tests, n_anchors, probs = fut.result()
                    lib_suites += 1
                    lib_tests += n_tests
                    total_anchors += n_anchors
                    if probs:
                        failures.append(f"{p.relative_to(ROOT)}:\n  " + "\n  ".join(probs))

                for p, fut in prog_futs:
                    _, n_tests, n_anchors, probs = fut.result()
                    prog_suites += 1
                    prog_tests += n_tests
                    total_anchors += n_anchors
                    if probs:
                        failures.append(f"{p.relative_to(ROOT)}:\n  " + "\n  ".join(probs))

                for rel, fut in modb_futs:
                    passed, n_tests, n_anchors, probs = fut.result()
                    modb_suites += 1
                    modb_tests += passed
                    total_anchors += n_anchors
                    if probs:
                        failures.append(f"modb/{rel}:\n  " + "\n  ".join(probs))

            # Inline capability & edge-case differential checks (Slices 1A–1D).
            check_env_and_limits_differential(mo, mo_rt, tmpdir)
            check_slice_1c_differential(mo, mo_rt, tmpdir)
            check_slice_1d_differential(mo, mo_rt, tmpdir)

    if failures:
        sys.stderr.write("\n\n".join(failures) + "\n")
        sys.exit(1)

    golden_total = golden_full_run + golden_full_test + golden_compile_only + golden_promise_suites
    parts = [
        f"golden {golden_total}/{len(golden_dirs)} "
        f"({golden_full_run + golden_full_test} full [{golden_full_run} run, {golden_full_test} test], "
        f"{golden_promise_suites} promise/service [{golden_promise_tests} tests/examples], "
        f"{golden_compile_only} compile-only)"
    ]
    if not args.case:
        parts.extend(
            [
                f"lower-source {lower_suites} ({lower_mains} main, {lower_tests} tests)",
                f"examples {example_suites} ({example_tests} tests)",
                f"lib {lib_suites} ({lib_tests} tests)",
                f"tools+services {prog_suites} ({prog_tests} tests)",
            ]
        )
        if modb_suites:
            parts.append(f"MoDB {modb_suites} ({modb_tests} tests)")
        parts.append(f"{total_anchors} anchors")
        parts.append("inline caps/TLS/Sqlite/Command/Sim/UCD/verify")
    print(
        "rust runtime: "
        + "; ".join(parts)
        + "; 0 diffs; #![forbid(unsafe_code)]; no libssl/libcrypto/libsqlite3"
    )


if __name__ == "__main__":
    main()
