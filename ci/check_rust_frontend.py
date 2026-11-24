#!/usr/bin/env python3
"""Differential test harness for the pure-Rust Mo front end (`mo-base`, `mo-syntax`, `mo-core`, `mo-fe`).

Run from the repository root:
    python3 ci/check_rust_frontend.py [--mo PATH] [--mo-fe PATH] [--mocore PATH]
                                      [--mo-grammar PATH] [--skip-build] [--require-lean]
                                      [--generated-count N] [--jobs N]

What this checks across Phase 2 Slices 2A and 2B:
  1. Workspace lint & safety policy audit (all 11 crates):
       - Root `clippy.toml` configures test exemptions without weakening library lints.
       - Root `Cargo.toml` enforces hardened `writing-good-rust` `[workspace.lints]`
         including `unsafe_code = "forbid"` and `unwrap_used = "deny"`.
       - Every crate under `crates/` opts into `[lints] workspace = true` and puts
         `#![forbid(unsafe_code)]` in the first 5 lines of every entry point (`lib.rs`,
         `main.rs`, `build.rs`).
  2. Lossless `.core` v0.11 canonical round-trip on all committed Lean fixtures (Slice 2A):
       - 15/15 `tests/core/*.core` (`Run`) via `mo-fe core-print` (both `--kind run` and
         `--kind auto`) and `mo-fe core-check`.
       - 20/20 `fuzz/corpus/fuzz_core_text/*` across all explicit `CoreTextKind`s and
         `--kind auto`.
  3. Core typechecker differential parity (`mo-fe core-check-batch` vs C `test_core_check.test`
     AND Lean `formal/.lake/build/bin/mocore`) across all cases in `ci/check_core_check.py`.
  4. Generated well-typed Core programs (`ci/gen_core.py`) 3-way check & canonical print parity.
  5. C `mo check --emit=core` front-end seam verification across real Mo programs.
  6. Grammar corpus parity (Slice 2B):
       - 106/106 `grammar/tests/` (`good`, `bad`, `bad-files`) match Bison `mo-grammar` 100%.
  7. Formatter byte-for-byte parity across all tracked `.mo` files (Slice 2B):
       - Every tracked `.mo` file (`git ls-files '*.mo'`, 1,559 files) matches C `mo format`
         accept/refuse and formatted bytes byte-for-byte, and a second `mo-fe format` pass is
         idempotent on every accepted file.
  8. Syntax, lexer, and formatter error diagnostic parity (Slice 2B):
       - Every syntax/lexer/format case in `tests/errors/` (including generated limit cases)
         matches `expected` stderr byte-for-byte and C `mo check --json` diagnostic records
         byte-for-byte.
"""

import argparse
import concurrent.futures
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "ci"))

import check_core_check  # noqa: E402
import check_core_generated  # noqa: E402
import gen_core  # noqa: E402

BATCH = 128

FUZZ_CORE_TEXT_KINDS: dict[str, str] = {
    "cap_state.core": "cap-state",
    "command.core": "primitive",
    "crypto.core": "primitive",
    "entries.core": "primitive",
    "fake_io.core": "primitive",
    "io.core": "primitive",
    "literal.core": "literal",
    "metadata.core": "program",
    "network.core": "primitive",
    "outcome.core": "outcome",
    "primitive.core": "primitive",
    "program.core": "program",
    "random_state.core": "random-state",
    "report.core": "report",
    "secure_random.core": "primitive",
    "simulation.core": "primitive",
    "sqlite.core": "primitive",
    "type.core": "type",
    "unicode16.core": "primitive",
    "value.core": "value",
}

REQUIRED_WORKSPACE_LINTS = (
    'unsafe_code = "forbid"',
    'unsafe_op_in_unsafe_fn = "deny"',
    'unwrap_used = "deny"',
    'expect_used = "warn"',
    'panic = "warn"',
    'todo = "deny"',
    'unimplemented = "deny"',
    'dbg_macro = "deny"',
    'allow_attributes = "deny"',
    'allow_attributes_without_reason = "deny"',
    'await_holding_lock = "deny"',
    'let_underscore_future = "deny"',
    'or_fun_call = "warn"',
    'undocumented_unsafe_blocks = "deny"',
    'large_futures = "warn"',
)

REQUIRED_CLIPPY_TOML = (
    "allow-unwrap-in-tests = true",
    "allow-expect-in-tests = true",
    "allow-dbg-in-tests = true",
)


def check_workspace_lint_policy() -> tuple[int, int]:
    """Audit clippy.toml, root Cargo.toml lints, and every crate's opt-in + forbid(unsafe_code)."""
    clippy_toml = ROOT / "clippy.toml"
    assert clippy_toml.is_file(), "root clippy.toml is missing"
    clippy_text = clippy_toml.read_text(encoding="utf-8")
    for line in REQUIRED_CLIPPY_TOML:
        assert line in clippy_text, f"clippy.toml missing required setting: {line}"

    cargo_text = (ROOT / "Cargo.toml").read_text(encoding="utf-8")
    for rule in REQUIRED_WORKSPACE_LINTS:
        assert rule in cargo_text, f"Cargo.toml missing required workspace lint: {rule}"

    crate_dirs = sorted(p for p in (ROOT / "crates").iterdir() if p.is_dir())
    assert len(crate_dirs) >= 11, f"expected >= 11 workspace crates, found {len(crate_dirs)}"
    entry_points_checked = 0
    for crate in crate_dirs:
        crate_toml = (crate / "Cargo.toml").read_text(encoding="utf-8")
        assert re.search(r"(?m)^\[lints\]\s*\nworkspace\s*=\s*true\b", crate_toml), (
            f"{crate.relative_to(ROOT)}/Cargo.toml is missing `[lints] workspace = true`"
        )
        entries = [
            p
            for p in (crate / "src/lib.rs", crate / "src/main.rs", crate / "build.rs")
            if p.is_file()
        ]
        assert entries, f"{crate.relative_to(ROOT)}: no Rust entry point found"
        for ep in entries:
            first_lines = ep.read_text(encoding="utf-8").splitlines()[:5]
            assert any(line.strip() == "#![forbid(unsafe_code)]" for line in first_lines), (
                f"{ep.relative_to(ROOT)} is missing #![forbid(unsafe_code)] in first 5 lines"
            )
            entry_points_checked += 1
    return len(crate_dirs), entry_points_checked


def find_or_build_mo(explicit: str | None) -> pathlib.Path:
    if explicit:
        path = pathlib.Path(explicit).resolve()
        assert path.is_file(), f"mo binary not found: {path}"
        return path
    for candidate in (
        ROOT / "build/gcc/debug/mo",
        ROOT / "build/clang/debug/mo",
        ROOT / "build/cc/debug/mo",
        ROOT / "build/gcc/release/mo",
        ROOT / "build/clang/release/mo",
        ROOT / "build/cc/release/mo",
    ):
        if candidate.is_file():
            return candidate
    subprocess.run(["make", "--no-print-directory", "-j8", "CC=gcc"], cwd=ROOT, check=True)
    built = ROOT / "build/gcc/debug/mo"
    assert built.is_file(), f"failed to build {built}"
    return built


def find_or_build_c_core_drivers(mo_bin: pathlib.Path) -> tuple[pathlib.Path, pathlib.Path]:
    """Locate or build C `test_core_check.test` and `test_core_print.test`."""
    # Prefer the debug tree matching mo_bin's compiler (`build/<cc>/debug/tests/...`).
    rel_parts = None
    try:
        rel_parts = mo_bin.resolve().relative_to(ROOT).parts
    except ValueError:
        pass
    compiler = rel_parts[1] if rel_parts and len(rel_parts) >= 3 and rel_parts[0] == "build" else "gcc"
    if compiler not in ("gcc", "clang", "cc"):
        compiler = "gcc"
    test_dir = ROOT / "build" / compiler / "debug" / "tests"
    check_bin = test_dir / "test_core_check.test"
    print_bin = test_dir / "test_core_print.test"
    missing = [str(p.relative_to(ROOT)) for p in (check_bin, print_bin) if not p.is_file()]
    if missing:
        subprocess.run(
            ["make", "--no-print-directory", "-j8", f"CC={compiler}", *missing],
            cwd=ROOT,
            check=True,
        )
    assert check_bin.is_file(), f"missing C driver: {check_bin}"
    assert print_bin.is_file(), f"missing C driver: {print_bin}"
    return check_bin, print_bin


def find_or_build_mo_fe(explicit: str | None, skip_build: bool) -> pathlib.Path:
    if explicit:
        path = pathlib.Path(explicit).resolve()
        assert path.is_file(), f"mo-fe binary not found: {path}"
        return path
    if not skip_build:
        subprocess.run(
            ["cargo", "build", "--release", "--bin", "mo-fe"],
            cwd=ROOT,
            check=True,
        )
    built = ROOT / "target/release/mo-fe"
    assert built.is_file(), f"mo-fe binary not found: {built}"
    return built


def find_or_build_mocore(
    explicit: str | None, skip_build: bool, require_lean: bool
) -> pathlib.Path | None:
    if explicit:
        path = pathlib.Path(explicit).resolve()
        assert path.is_file(), f"mocore binary not found: {path}"
        return path
    env_override = os.environ.get("MO_MOCORE")
    if env_override:
        path = pathlib.Path(env_override).resolve()
        assert path.is_file(), f"MO_MOCORE not found: {path}"
        return path
    default_bin = ROOT / "formal/.lake/build/bin/mocore"
    if default_bin.is_file():
        return default_bin

    lake = shutil.which("lake")
    if lake is None:
        for base in (os.environ.get("ELAN_HOME"), "/opt/elan", str(pathlib.Path.home() / ".elan")):
            if not base:
                continue
            cand = pathlib.Path(base) / "bin" / "lake"
            if cand.is_file() and os.access(cand, os.X_OK):
                lake = str(cand)
                break
    if lake is not None and not skip_build:
        env = os.environ.copy()
        lake_dir = str(pathlib.Path(lake).parent)
        env["PATH"] = f"{lake_dir}:{env.get('PATH', '')}"
        subprocess.run([lake, "build", "mocore"], cwd=ROOT / "formal", env=env, check=True)
        if default_bin.is_file():
            return default_bin

    if require_lean:
        raise AssertionError(
            "Lean mocore binary not found and elan/lake is unavailable (pass --mocore or install elan)"
        )
    return None


def check_committed_fixtures_and_fuzz_seeds(mo_fe: pathlib.Path) -> tuple[int, int]:
    """Verify byte-for-byte canonical round-trip on 15/15 tests/core and 20/20 fuzz_core_text."""
    core_dir = ROOT / "tests/core"
    core_files = sorted(p for p in core_dir.iterdir() if p.is_file() and p.suffix == ".core")
    assert len(core_files) == 15, f"expected 15 tests/core/*.core files, found {len(core_files)}"
    for path in core_files:
        raw = path.read_bytes()
        for kind in ("run", "auto"):
            res = subprocess.run(
                [str(mo_fe), "core-print", "--kind", kind, str(path)],
                cwd=ROOT,
                capture_output=True,
                timeout=30,
            )
            assert res.returncode == 0, (
                f"mo-fe core-print --kind {kind} failed on {path.name}: "
                f"{res.stderr.decode(errors='replace')}"
            )
            assert res.stdout == raw, f"round-trip byte diff on {path.name} (--kind {kind})"
        chk = subprocess.run(
            [str(mo_fe), "core-check", "--kind", "run", str(path)],
            cwd=ROOT,
            capture_output=True,
            timeout=30,
        )
        assert chk.returncode == 0 and chk.stderr == b"", (
            f"mo-fe core-check failed on {path.name}: {chk.stderr.decode(errors='replace')}"
        )

    fuzz_dir = ROOT / "fuzz/corpus/fuzz_core_text"
    actual = sorted(p.name for p in fuzz_dir.iterdir() if p.is_file())
    expected = sorted(FUZZ_CORE_TEXT_KINDS)
    assert actual == expected, f"fuzz_core_text inventory changed: actual={actual}, expected={expected}"
    for name, kind in sorted(FUZZ_CORE_TEXT_KINDS.items()):
        path = fuzz_dir / name
        raw = path.read_bytes()
        for mode in (kind, "auto"):
            res = subprocess.run(
                [str(mo_fe), "core-print", "--kind", mode, str(path)],
                cwd=ROOT,
                capture_output=True,
                timeout=30,
            )
            assert res.returncode == 0, (
                f"mo-fe core-print --kind {mode} failed on fuzz_core_text/{name}: "
                f"{res.stderr.decode(errors='replace')}"
            )
            assert res.stdout == raw, f"round-trip byte diff on fuzz_core_text/{name} (--kind {mode})"
    return len(core_files), len(actual)


def collect_core_check_cases() -> tuple[list[tuple[str, bytes, int | None]], int, int]:
    """Build the exact differential corpus from ci/check_core_check.py."""
    cases: list[tuple[str, bytes, int | None]] = (
        list(check_core_check.generated())
        + list(check_core_check.typed_trees())
        + list(check_core_check.rule_cases())
    )
    roots = [
        ROOT / "tests/core",
        ROOT / "fuzz/corpus/fuzz_core_text",
        ROOT / "fuzz/corpus/fuzz_core_check",
    ]
    originals = programs = 0
    for root in roots:
        if not root.exists():
            continue
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            raw = path.read_bytes()
            assert len(raw) <= (8 << 20), f"oversize corpus input: {path}"
            try:
                doc = check_core_check.parse(raw.decode("utf-8"))
                payload = (
                    doc[2]
                    if doc[:2] == [check_core_check.Atom("core"), check_core_check.Atom(check_core_check.VERSION)]
                    else None
                )
            except (ValueError, IndexError, RecursionError, UnicodeError, OverflowError):
                payload = None
            rel_label = str(path.relative_to(ROOT))
            if payload and payload[0] in ("Run", "Program"):
                p = payload[1] if payload[0] == "Run" and len(payload) > 1 else payload
                tokens = [
                    m
                    for m in re.finditer(rb';[^\n]*|"(?:\\.|[^"\\])*"|[()]|[^\s();"]+', raw)
                    if not m[0].startswith(b";")
                ]
                if payload[0] == "Run":
                    if (
                        len(payload) == 5
                        and isinstance(payload[-1], check_core_check.Atom)
                        and re.fullmatch(r"0|[1-9][0-9]*", payload[-1])
                    ):
                        fuel = tokens[-3]
                        raw = raw[: fuel.start()] + b"0" + raw[fuel.end() :]
                    cases.append((rel_label, raw, None))
                else:
                    cases.append(
                        (
                            rel_label,
                            check_core_check.run(
                                raw[tokens[3].start() : tokens[-2].end()].decode("utf-8")
                            ),
                            None,
                        )
                    )
                for label, q in check_core_check.mutate(p):
                    cases.append((f"{rel_label}/{label}", check_core_check.run(check_core_check.wire(q)), None))
                programs += 1
            else:
                cases.append((rel_label, raw, None))
            originals += 1
    return cases, originals, programs


def run_batch_lines(cmd_prefix: list[str], files: list[str], jobs: int) -> list[str]:
    batches = [files[start : start + BATCH] for start in range(0, len(files), BATCH)]

    def one(batch: list[str]) -> list[str]:
        res = subprocess.run(
            [*cmd_prefix, *batch],
            capture_output=True,
            text=True,
            check=True,
            timeout=60,
        )
        return res.stdout.splitlines()

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        out: list[str] = []
        for lines in pool.map(one, batches):
            out.extend(lines)
    return out


def run_batch_printed(cmd_prefix: list[str], files: list[str], jobs: int) -> list[str]:
    batches = [files[start : start + BATCH] for start in range(0, len(files), BATCH)]

    def one(batch: list[str]) -> list[str]:
        res = subprocess.run(
            [*cmd_prefix, *batch],
            capture_output=True,
            check=True,
            timeout=120,
        )
        return check_core_generated.printed(res.stdout)

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        out: list[str] = []
        for texts in pool.map(one, batches):
            out.extend(texts)
    return out


def check_core_checker_differential(
    mo_fe: pathlib.Path,
    c_check: pathlib.Path,
    mocore: pathlib.Path | None,
    jobs: int,
) -> tuple[int, int, int]:
    """Compare mo-fe core-check verdicts & error codes against C and Lean mocore."""
    cases, originals, programs = collect_core_check_cases()
    with tempfile.TemporaryDirectory(prefix="mo-rust-core-check-") as tmp:
        files: list[str] = []
        for i, (_, raw, _) in enumerate(cases):
            p = pathlib.Path(tmp) / f"{i}.core"
            p.write_bytes(raw)
            files.append(str(p))

        rs_lines = run_batch_lines([str(mo_fe), "core-check-batch", "--kind", "run"], files, jobs)
        c_lines = run_batch_lines([str(c_check)], files, jobs)
        assert len(rs_lines) == len(cases), f"mo-fe returned {len(rs_lines)} of {len(cases)} verdicts"
        assert len(c_lines) == len(cases), f"C returned {len(c_lines)} of {len(cases)} verdicts"

        lean_verdicts: list[int] | None = None
        if mocore is not None:
            def one_lean(item: tuple[str, tuple[str, bytes, int | None]]) -> int:
                path, (label, raw, want) = item
                res = subprocess.run([str(mocore), path], capture_output=True, timeout=20)
                verdict = res.returncode if res.returncode in (4, 5) else 0
                assert res.returncode in (0, 1, 2, 3, 4, 5, 6), (
                    f"{label}: mocore failed ({res.returncode}): {res.stdout!r} {res.stderr!r}"
                )
                if want is not None:
                    assert verdict == want, (
                        f"{label}: generator expected {want}, Lean gave {verdict}: {raw!r}"
                    )
                return verdict

            with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
                lean_verdicts = list(pool.map(one_lean, zip(files, cases)))

        for i, ((label, raw, want), rs_line, c_line) in enumerate(zip(cases, rs_lines, c_lines)):
            rs_parts = rs_line.split()
            c_parts = c_line.split()
            rs_status = int(rs_parts[0])
            c_status = int(c_parts[0])

            if want is not None:
                assert rs_status == want, (
                    f"{label}: generator expected {want}, mo-fe gave {rs_line!r}\n"
                    f"{raw.decode(errors='replace')}"
                )
            assert rs_status == c_status, (
                f"{label}: verdict mismatch: C={c_line!r}, mo-fe={rs_line!r}\n"
                f"{raw.decode(errors='replace')}"
            )
            if lean_verdicts is not None:
                assert rs_status == lean_verdicts[i], (
                    f"{label}: verdict mismatch vs Lean ({lean_verdicts[i]}): "
                    f"mo-fe={rs_line!r}, C={c_line!r}\n{raw.decode(errors='replace')}"
                )
            if rs_status in (4, 5):
                assert len(rs_parts) >= 2 and len(c_parts) >= 2, (
                    f"{label}: missing error code in output: C={c_line!r}, mo-fe={rs_line!r}"
                )
                assert rs_parts[1] == c_parts[1], (
                    f"{label}: error code mismatch: C={c_line!r}, mo-fe={rs_line!r}\n"
                    f"{raw.decode(errors='replace')}"
                )
                if rs_status == 5:
                    code_num = int(rs_parts[1][1:])
                    assert code_num not in (1052, 1053), (
                        f"{label}: resource refusal is not a typing verdict: {rs_line}"
                    )
    return len(cases), originals, programs


def check_generated_core_programs(
    mo_fe: pathlib.Path,
    c_check: pathlib.Path,
    c_print: pathlib.Path,
    mocore: pathlib.Path | None,
    count: int,
    jobs: int,
) -> int:
    """Verify generated well-typed Core programs pass Rust/C/Lean checkers and print byte-identically."""
    assert count >= 1, "--generated-count must be >= 1"
    texts = [gen_core.document(seed) for seed in range(count)]
    if count >= 200:
        joined = "".join(texts)
        for form in check_core_generated.COVERED:
            assert f"({form} " in joined, f"no generated program uses ({form} ...)"

    with tempfile.TemporaryDirectory(prefix="mo-rust-core-gen-") as tmp:
        bad_path = pathlib.Path(tmp) / "ill-typed.core"
        bad_path.write_bytes(check_core_generated.ILL_TYPED.encode("utf-8"))
        rs_bad = subprocess.run(
            [str(mo_fe), "core-check-batch", "--kind", "run", str(bad_path)],
            capture_output=True,
            text=True,
            check=True,
        )
        assert rs_bad.stdout.startswith("5 E1036 "), (
            f"mo-fe failed negative control: {rs_bad.stdout!r}"
        )

        files: list[str] = []
        for i, text in enumerate(texts):
            p = pathlib.Path(tmp) / f"{i}.core"
            p.write_bytes(text.encode("utf-8"))
            files.append(str(p))

        rs_verdicts = run_batch_lines([str(mo_fe), "core-check-batch", "--kind", "run"], files, jobs)
        c_verdicts = run_batch_lines([str(c_check)], files, jobs)
        for i, (rv, cv) in enumerate(zip(rs_verdicts, c_verdicts)):
            assert rv == "0" and cv == "0", (
                f"generated seed {i} refused: mo-fe={rv!r}, C={cv!r}\n{texts[i]}"
            )

        rs_printed = run_batch_printed([str(mo_fe), "core-print-batch", "--kind", "run"], files, jobs)
        c_printed = run_batch_printed([str(c_print)], files, jobs)
        assert len(rs_printed) == count and len(c_printed) == count
        for i, (want, got_rs, got_c) in enumerate(zip(texts, rs_printed, c_printed)):
            assert got_rs == want, f"mo-fe print mismatch on seed {i}\nwant: {want}\ngot:  {got_rs}"
            assert got_c == want, f"C print mismatch on seed {i}\nwant: {want}\ngot:  {got_c}"

        if mocore is not None:
            def lean_one(item: tuple[int, str]) -> None:
                i, path = item
                rc, out = check_core_generated.lean(str(mocore), [path])
                assert rc in (0, 1, 2, 3), (
                    f"mocore refused generated seed {i} (exit {rc}): {out}\n{texts[i]}"
                )
                rc2, canon = check_core_generated.lean(str(mocore), ["--canonical", path])
                assert rc2 == 0 and canon == texts[i], (
                    f"mocore canonical print mismatch on seed {i} (exit {rc2})"
                )

            with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
                list(pool.map(lean_one, enumerate(files)))
    return count


def discover_mo_programs() -> list[tuple[str, str]]:
    """Collect real `.mo` entry points across examples/, tests/lower-source/, and tests/golden/."""
    programs: list[tuple[str, str]] = []

    # 1. All examples/
    examples_dir = ROOT / "examples"
    for p in sorted(examples_dir.glob("*.mo")):
        programs.append((f"example/{p.stem}", str(p.relative_to(ROOT))))
    for d in sorted(p for p in examples_dir.iterdir() if p.is_dir()):
        main_mo = d / "main.mo"
        if main_mo.is_file():
            programs.append((f"example/{d.name}", str(main_mo.relative_to(ROOT))))

    # 2. All tests/lower-source/*/main.mo
    for d in sorted(p for p in (ROOT / "tests/lower-source").iterdir() if p.is_dir()):
        main_mo = d / "main.mo"
        if main_mo.is_file():
            programs.append((f"lower-source/{d.name}", str(main_mo.relative_to(ROOT))))

    # 3. All non-compile-error tests/golden/*
    for d in sorted(p for p in (ROOT / "tests/golden").iterdir() if p.is_dir()):
        exp_stdout = (d / "stdout").read_bytes() if (d / "stdout").exists() else b""
        exp_stderr = (d / "stderr").read_bytes() if (d / "stderr").exists() else b""
        exp_status = int((d / "status").read_text().strip()) if (d / "status").exists() else 0
        if exp_status != 0 and exp_stdout == b"" and exp_stderr.startswith(b"error E"):
            continue
        entry = (d / "entry").read_text().strip() if (d / "entry").exists() else None
        rel = entry or str((d / "main.mo").relative_to(ROOT))
        if (ROOT / rel).is_file():
            programs.append((f"golden/{d.name}", rel))

    return programs


def check_c_emitted_core_programs(
    mo_bin: pathlib.Path,
    mo_fe: pathlib.Path,
    c_check: pathlib.Path,
    jobs: int,
) -> tuple[int, int]:
    """Emit `.core` via C `mo check --emit=core`, verify byte-exact round-trip and C==Rust verdicts."""
    entries = discover_mo_programs()
    assert len(entries) >= 250, f"expected >= 250 real .mo programs, found {len(entries)}"

    with tempfile.TemporaryDirectory(prefix="mo-rust-emit-core-") as tmp:
        tmp_path = pathlib.Path(tmp)

        def emit_one(item: tuple[int, tuple[str, str]]) -> tuple[str, pathlib.Path, pathlib.Path, bytes] | None:
            idx, (label, rel_prog) = item
            res = subprocess.run(
                [str(mo_bin), "check", "--emit=core", rel_prog],
                cwd=ROOT,
                capture_output=True,
                timeout=60,
            )
            if res.returncode != 0:
                # Some golden programs intentionally fail a checked anchor (`mo check` runs anchors).
                if label.startswith("golden/"):
                    return None
                raise AssertionError(
                    f"C mo check --emit=core failed on {label} ({rel_prog}): "
                    f"{res.stderr.decode(errors='replace')}"
                )
            raw = res.stdout
            assert raw.startswith(f"(core {check_core_check.VERSION} (Program ".encode()), (
                f"unexpected --emit=core output on {label}: {raw[:120]!r}"
            )
            prog_file = tmp_path / f"prog_{idx}.core"
            prog_file.write_bytes(raw)

            # Also wrap as Run so C `test_core_check.test` can verify the exact same Program.
            # Strip outer "(core 0.11 " prefix and trailing ")\n".
            prefix = f"(core {check_core_check.VERSION} ".encode()
            inner = raw[len(prefix) :].rstrip()
            assert inner.endswith(b")"), f"malformed core envelope on {label}"
            program_sexp = inner[:-1].decode("utf-8")
            run_file = tmp_path / f"run_{idx}.core"
            run_file.write_bytes(check_core_check.run(program_sexp))
            return label, prog_file, run_file, raw

        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            emitted = [item for item in pool.map(emit_one, enumerate(entries)) if item is not None]

        assert len(emitted) >= 250, f"expected >= 250 emitted .core programs, got {len(emitted)}"

        prog_files = [str(p) for _, p, _, _ in emitted]
        run_files = [str(r) for _, _, r, _ in emitted]

        # 1. Byte-for-byte canonical round-trip on every emitted Program.
        printed = run_batch_printed(
            [str(mo_fe), "core-print-batch", "--kind", "program"], prog_files, jobs
        )
        assert len(printed) == len(emitted)
        for (label, _, _, raw), got in zip(emitted, printed):
            assert got.encode("utf-8") == raw, f"mo-fe core-print round-trip diff on {label}"

        # 2. Exact verdict & error-code parity between Rust mo-fe and C test_core_check.
        rs_verdicts = run_batch_lines(
            [str(mo_fe), "core-check-batch", "--kind", "program"], prog_files, jobs
        )
        c_verdicts = run_batch_lines([str(c_check)], run_files, jobs)
        accepted = 0
        for (label, _, _, raw), rv, cv in zip(emitted, rs_verdicts, c_verdicts):
            rs_parts = rv.split()
            c_parts = cv.split()
            assert rs_parts[0] == c_parts[0], (
                f"{label}: verdict mismatch on emitted Core: C={cv!r}, mo-fe={rv!r}"
            )
            if rs_parts[0] == "0":
                accepted += 1
            else:
                # Only service programs with out-of-band `mo_services` descriptors (`sim (start/send)`)
                # may fail standalone Core checking (and both C and Rust must give E1037).
                assert b"(sim (start " in raw or b"(sim (send " in raw, (
                    f"{label}: unexpected Core refusal without service descriptors: {rv!r}"
                )
                assert rs_parts[1] == c_parts[1] == "E1037", (
                    f"{label}: unexpected code mismatch on service Core: C={cv!r}, mo-fe={rv!r}"
                )
        return len(emitted), accepted


SYNTAX_AND_FORMAT_ERROR_CODES = (
    set(range(1, 34)) | {96, 97, 1001} | set(range(1005, 1012)) | {1402, 1403}
)


def _write_error_file(folder: pathlib.Path, text: str, name: str = "main.mo") -> None:
    (folder / name).write_text(text, encoding="utf-8")


SYNTAX_ERROR_GENERATORS = {
    "file-too-big": lambda f: _write_error_file(f, "module Main\n" + "# padding\n" * 1_000_000),
    "line-too-long": lambda f: _write_error_file(f, "module Main\n# " + "x" * 70_000 + "\n"),
    "nested-too-deeply": lambda f: _write_error_file(
        f, "module Main\nfn run() : Int\n" + "(" * 300 + "1" + ")" * 300 + "\nend\n"
    ),
    "interpolations-too-deep": lambda f: _write_error_file(
        f, "module Main\nfn run() : Text\n" + '"#{' * 20 + "1" + '}"' * 20 + "\nend\n"
    ),
    "too-many-names": lambda f: _write_error_file(
        f, "module Main\n" + "".join(f"x{i} = {i}\n" for i in range(65536))
    ),
    "too-many-parameters": lambda f: _write_error_file(
        f,
        f"module Main\nfn f({', '.join(f'a{i}: Int' for i in range(256))}) : Int\n1\nend\n",
    ),
    "too-many-fields": lambda f: _write_error_file(
        f, "module Main\nstruct S\n" + "".join(f"f{i}: Int\n" for i in range(65536)) + "end\n"
    ),
    "too-many-cases": lambda f: _write_error_file(
        f, "module Main\nenum E\n" + "".join(f"C{i}\n" for i in range(65536)) + "end\n"
    ),
    "format-limit": lambda f: _write_error_file(
        f,
        "module Main\nfn run() : Int\n  [\n"
        + ",\n".join(["[1,2,3]"] * 100_000)
        + "\n].size\nend\n",
    ),
}


def find_or_build_mo_grammar(
    explicit: str | None, mo_bin: pathlib.Path, skip_build: bool, require_bison: bool
) -> pathlib.Path | None:
    """Locate or build the Bison executable grammar (`build/<cc>/debug/mo-grammar`)."""
    if explicit:
        path = pathlib.Path(explicit).resolve()
        assert path.is_file(), f"mo-grammar binary not found: {path}"
        return path
    rel_parts = None
    try:
        rel_parts = mo_bin.resolve().relative_to(ROOT).parts
    except ValueError:
        pass
    compiler = (
        rel_parts[1]
        if rel_parts and len(rel_parts) >= 3 and rel_parts[0] == "build"
        else "gcc"
    )
    if compiler not in ("gcc", "clang", "cc"):
        compiler = "gcc"
    target = ROOT / "build" / compiler / "debug" / "mo-grammar"
    if target.is_file():
        return target
    if not skip_build and shutil.which("bison") is not None:
        subprocess.run(
            ["make", "--no-print-directory", "-j8", f"CC={compiler}", str(target.relative_to(ROOT))],
            cwd=ROOT,
            check=True,
        )
        if target.is_file():
            return target
    if require_bison:
        raise AssertionError(
            "Bison mo-grammar binary not found and bison is unavailable (pass --mo-grammar or install bison)"
        )
    return None


def decode_format_batch(data: bytes) -> list[tuple[bool, bytes | int]]:
    """Decode `mo-fe format-batch` framed output: `R <len>\\n<bytes>` or `E <code>\\n`."""
    results: list[tuple[bool, bytes | int]] = []
    pos = 0
    end = len(data)
    while pos < end:
        nl = data.index(b"\n", pos)
        header = data[pos:nl].decode("ascii")
        pos = nl + 1
        kind, value = header.split(" ", 1)
        if kind == "R":
            length = int(value)
            payload = data[pos : pos + length]
            assert len(payload) == length, "truncated format-batch payload"
            pos += length
            results.append((True, payload))
        elif kind == "E":
            results.append((False, int(value)))
        else:
            raise AssertionError(f"unexpected format-batch header: {header!r}")
    return results


def run_format_batch(
    mo_fe: pathlib.Path, files: list[str], jobs: int, fragment: bool = False
) -> list[tuple[bool, bytes | int]]:
    batches = [files[start : start + BATCH] for start in range(0, len(files), BATCH)]
    prefix = [str(mo_fe), "format-batch", *(["--fragment"] if fragment else [])]

    def one(batch: list[str]) -> list[tuple[bool, bytes | int]]:
        res = subprocess.run(
            [*prefix, *batch],
            capture_output=True,
            check=True,
            timeout=120,
        )
        return decode_format_batch(res.stdout)

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        out: list[tuple[bool, bytes | int]] = []
        for items in pool.map(one, batches):
            out.extend(items)
    return out


def check_grammar_corpus_parity(
    mo_fe: pathlib.Path, mo_grammar: pathlib.Path | None
) -> tuple[int, int, int]:
    """Verify 106/106 grammar/tests/ (good, bad, bad-files) match Bison mo-grammar 100%."""
    good = sorted((ROOT / "grammar/tests/good").glob("*.mo"))
    bad = sorted((ROOT / "grammar/tests/bad").glob("*.mo"))
    bad_files = sorted((ROOT / "grammar/tests/bad-files").glob("*.mo"))
    total = len(good) + len(bad) + len(bad_files)
    assert len(good) == 13 and len(bad) == 90 and len(bad_files) == 3, (
        f"unexpected grammar/tests inventory: good={len(good)}, bad={len(bad)}, bad-files={len(bad_files)}"
    )

    good_lines = run_batch_lines(
        [str(mo_fe), "parse-batch", "--fragment"], [str(p) for p in good], jobs=1
    )
    bad_lines = run_batch_lines(
        [str(mo_fe), "parse-batch", "--fragment"], [str(p) for p in bad], jobs=1
    )
    bad_file_lines = run_batch_lines(
        [str(mo_fe), "parse-batch"], [str(p) for p in bad_files], jobs=1
    )

    for p, line in zip(good, good_lines):
        assert line == "0", f"mo-fe rejected good grammar fixture {p.relative_to(ROOT)}: {line!r}"
    for p, line in zip(bad, bad_lines):
        assert line.startswith("1 "), f"mo-fe accepted bad grammar fixture {p.relative_to(ROOT)}: {line!r}"
    for p, line in zip(bad_files, bad_file_lines):
        assert line.startswith("1 "), (
            f"mo-fe accepted bad-files grammar fixture {p.relative_to(ROOT)}: {line!r}"
        )

    if mo_grammar is not None:
        for paths, frag, want_ok in (
            (good, True, True),
            (bad, True, False),
            (bad_files, False, False),
        ):
            for p in paths:
                cmd = [str(mo_grammar), *(["--fragment"] if frag else []), str(p)]
                res = subprocess.run(cmd, cwd=ROOT, capture_output=True, timeout=30)
                bison_ok = res.returncode == 0
                assert bison_ok == want_ok, (
                    f"Bison mo-grammar mismatch on {p.relative_to(ROOT)}: "
                    f"expected {want_ok}, got rc={res.returncode} ({res.stderr.decode(errors='replace')})"
                )
    return total, len(good), len(bad) + len(bad_files)


def check_tracked_mo_format_parity(
    mo_bin: pathlib.Path, mo_fe: pathlib.Path, jobs: int
) -> tuple[int, int, int]:
    """Compare mo-fe format vs C mo format byte-for-byte across all tracked .mo files + idempotence."""
    tracked = [
        p
        for p in subprocess.check_output(
            ["git", "ls-files", "*.mo"], cwd=ROOT, text=True
        ).splitlines()
        if p
    ]
    assert len(tracked) >= 1500, f"expected >= 1500 tracked .mo files, found {len(tracked)}"
    abs_paths = [str(ROOT / rel) for rel in tracked]

    rs_results = run_format_batch(mo_fe, abs_paths, jobs=jobs, fragment=False)
    assert len(rs_results) == len(tracked)

    def run_c_one(rel: str) -> tuple[bool, bytes, bytes]:
        raw = (ROOT / rel).read_bytes()
        res = subprocess.run(
            [str(mo_bin), "format", "--stdin"],
            cwd=ROOT,
            input=raw,
            capture_output=True,
            timeout=60,
        )
        return res.returncode == 0, res.stdout, res.stderr

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        c_results = list(pool.map(run_c_one, tracked))

    accepted_outputs: list[tuple[str, bytes]] = []
    refused = 0
    for rel, (rs_ok, rs_payload), (c_ok, c_out, c_err) in zip(tracked, rs_results, c_results):
        assert rs_ok == c_ok, (
            f"format accept/refuse mismatch on {rel}: C_ok={c_ok} (stderr={c_err[:200]!r}), "
            f"Rust_ok={rs_ok} (code={rs_payload!r})"
        )
        if rs_ok:
            assert isinstance(rs_payload, bytes)
            assert rs_payload == c_out, (
                f"byte-for-byte format diff on {rel}: C={len(c_out)}B, Rust={len(rs_payload)}B"
            )
            accepted_outputs.append((rel, rs_payload))
        else:
            refused += 1

    # Verify idempotence: formatting every accepted output a second time yields identical bytes.
    with tempfile.TemporaryDirectory(prefix="mo-rust-format-idemp-") as tmp:
        tmp_path = pathlib.Path(tmp)
        second_files: list[str] = []
        for idx, (_, content) in enumerate(accepted_outputs):
            p = tmp_path / f"{idx}.mo"
            p.write_bytes(content)
            second_files.append(str(p))
        second_results = run_format_batch(mo_fe, second_files, jobs=jobs, fragment=False)
        assert len(second_results) == len(accepted_outputs)
        for (rel, once), (ok2, twice) in zip(accepted_outputs, second_results):
            assert ok2 and twice == once, f"mo-fe format is not idempotent on {rel}"

    return len(tracked), len(accepted_outputs), refused


def check_syntax_and_format_error_diagnostics(
    mo_bin: pathlib.Path, mo_fe: pathlib.Path, jobs: int
) -> tuple[int, int]:
    """Verify byte-for-byte stderr & JSON diagnostic parity on all syntax/lexer/format tests/errors/."""
    errors_dir = ROOT / "tests/errors"
    cases: list[pathlib.Path] = []
    for case in sorted(p for p in errors_dir.iterdir() if p.is_dir()):
        match = re.fullmatch(r"e(\d{4})(-[a-z0-9-]+)?", case.name)
        assert match, f"unexpected tests/errors entry: {case.name}"
        if int(match.group(1)) in SYNTAX_AND_FORMAT_ERROR_CODES:
            cases.append(case)
    assert len(cases) >= 60, f"expected >= 60 syntax/format error cases, found {len(cases)}"

    def run_one(case: pathlib.Path) -> tuple[str, bool]:
        cmd_name = (case / "command").read_text().strip() if (case / "command").exists() else "check"
        mofe_cmd = "format" if cmd_name == "format" else "parse"
        arguments = (case / "args").read_text().splitlines() if (case / "args").exists() else ["{main}"]
        exp_err = (case / "expected").read_bytes()
        exp_out = (case / "stdout").read_bytes() if (case / "stdout").exists() else b""
        gen = case / "generated"

        if gen.exists():
            gen_name = gen.read_text().strip()
            assert gen_name in SYNTAX_ERROR_GENERATORS, (
                f"{case.name}: missing generator {gen_name!r} in SYNTAX_ERROR_GENERATORS"
            )
            with tempfile.TemporaryDirectory(prefix="mo-rust-err-") as folder:
                SYNTAX_ERROR_GENERATORS[gen_name](pathlib.Path(folder))
                args = [a.replace("{main}", "main.mo") for a in arguments]
                res = subprocess.run(
                    [str(mo_fe), mofe_cmd, *args], cwd=folder, capture_output=True, timeout=120
                )
                assert res.returncode == 1, f"{case.name}: expected rc=1, got {res.returncode}"
                assert res.stdout == exp_out, f"{case.name}: stdout mismatch"
                assert res.stderr == exp_err, (
                    f"{case.name}: stderr mismatch\n--- expected\n{exp_err.decode(errors='replace')}\n"
                    f"--- actual\n{res.stderr.decode(errors='replace')}"
                )
                if mofe_cmd == "parse":
                    rj = subprocess.run(
                        [str(mo_fe), "parse", "--json", *args],
                        cwd=folder,
                        capture_output=True,
                        timeout=120,
                    )
                    cj = subprocess.run(
                        [str(mo_bin), "check", "--json", *args],
                        cwd=folder,
                        capture_output=True,
                        timeout=120,
                    )
                    c_diags = [
                        ln
                        for ln in cj.stdout.splitlines()
                        if json.loads(ln).get("record") not in ("version", "totals")
                    ]
                    r_diags = rj.stderr.splitlines()
                    assert rj.returncode == 1 and r_diags == c_diags, (
                        f"{case.name}: JSON diagnostic mismatch\nC={c_diags!r}\nRust={r_diags!r}"
                    )
                    return case.name, True
                return case.name, False

        main = str((case / "main.mo").relative_to(ROOT))
        args = [a.replace("{main}", main) for a in arguments]
        res = subprocess.run(
            [str(mo_fe), mofe_cmd, *args], cwd=ROOT, capture_output=True, timeout=120
        )
        assert res.returncode == 1, f"{case.name}: expected rc=1, got {res.returncode}"
        assert res.stdout == exp_out, f"{case.name}: stdout mismatch"
        assert res.stderr == exp_err, (
            f"{case.name}: stderr mismatch\n--- expected\n{exp_err.decode(errors='replace')}\n"
            f"--- actual\n{res.stderr.decode(errors='replace')}"
        )
        if mofe_cmd == "parse":
            rj = subprocess.run(
                [str(mo_fe), "parse", "--json", *args],
                cwd=ROOT,
                capture_output=True,
                timeout=120,
            )
            cj = subprocess.run(
                [str(mo_bin), "check", "--json", *args],
                cwd=ROOT,
                capture_output=True,
                timeout=120,
            )
            c_diags = [
                ln
                for ln in cj.stdout.splitlines()
                if json.loads(ln).get("record") not in ("version", "totals")
            ]
            r_diags = rj.stderr.splitlines()
            assert rj.returncode == 1 and r_diags == c_diags, (
                f"{case.name}: JSON diagnostic mismatch\nC={c_diags!r}\nRust={r_diags!r}"
            )
            return case.name, True
        return case.name, False

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        outcomes = list(pool.map(run_one, cases))
    json_checked = sum(1 for _, did_json in outcomes if did_json)
    return len(outcomes), json_checked


def main() -> None:
    started = time.monotonic()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mo", help="Path to built C mo executable")
    parser.add_argument("--mo-fe", help="Path to built Rust mo-fe executable")
    parser.add_argument("--mocore", help="Path to built Lean mocore executable")
    parser.add_argument("--mo-grammar", help="Path to built Bison mo-grammar executable")
    parser.add_argument("--skip-build", action="store_true", help="Do not build mo-fe, mocore, or mo-grammar")
    parser.add_argument(
        "--require-lean",
        action="store_true",
        help="Fail if Lean mocore is unavailable (also enforced when CI=1)",
    )
    parser.add_argument(
        "--generated-count",
        type=int,
        default=int(os.environ.get("MO_RUST_CORE_GENERATED", "200")),
        help="Number of ci/gen_core.py programs to cross-check (default 200)",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=min(8, os.cpu_count() or 2),
        help="Worker threads for differential runs",
    )
    args = parser.parse_args()
    require_tools = args.require_lean or os.environ.get("CI") == "1"

    crates_n, entries_n = check_workspace_lint_policy()
    mo_bin = find_or_build_mo(args.mo)
    c_check, c_print = find_or_build_c_core_drivers(mo_bin)
    mo_fe = find_or_build_mo_fe(args.mo_fe, args.skip_build)
    mocore = find_or_build_mocore(args.mocore, args.skip_build, require_tools)
    mo_grammar = find_or_build_mo_grammar(args.mo_grammar, mo_bin, args.skip_build, require_tools)

    runs_n, fuzz_n = check_committed_fixtures_and_fuzz_seeds(mo_fe)
    diff_n, orig_n, prog_n = check_core_checker_differential(mo_fe, c_check, mocore, args.jobs)
    gen_n = check_generated_core_programs(
        mo_fe, c_check, c_print, mocore, args.generated_count, args.jobs
    )
    emitted_n, accepted_n = check_c_emitted_core_programs(mo_bin, mo_fe, c_check, args.jobs)

    gram_n, gram_good, gram_bad = check_grammar_corpus_parity(mo_fe, mo_grammar)
    tracked_n, fmt_ok, fmt_refused = check_tracked_mo_format_parity(mo_bin, mo_fe, args.jobs)
    err_text_n, err_json_n = check_syntax_and_format_error_diagnostics(mo_bin, mo_fe, args.jobs)

    oracle_desc = "C + Lean mocore" if mocore is not None else "C (Lean mocore unavailable)"
    grammar_desc = "Bison mo-grammar" if mo_grammar is not None else "expected verdicts"
    print(
        f"Rust front-end 2A+2B parity ({oracle_desc}, {grammar_desc}): "
        f"{crates_n} crates/{entries_n} entry points lint-audited; "
        f"{runs_n}/15 runs + {fuzz_n}/20 fuzz seeds round-tripped; "
        f"{diff_n} Core checker differential cases ({orig_n} corpus files, {prog_n} programs) matched; "
        f"{gen_n} generated Core programs matched & printed identically; "
        f"{emitted_n} C-emitted .core programs round-tripped ({accepted_n} standalone-checked); "
        f"{gram_n}/106 grammar fixtures ({gram_good} good, {gram_bad} bad) matched; "
        f"{tracked_n} tracked .mo files matched C mo format byte-for-byte ({fmt_ok} formatted+idempotent, {fmt_refused} refused); "
        f"{err_text_n} syntax/format error diagnostics ({err_json_n} JSON) matched byte-for-byte; "
        f"{time.monotonic() - started:.2f}s"
    )


if __name__ == "__main__":
    main()
