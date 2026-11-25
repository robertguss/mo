#!/usr/bin/env python3
"""Differential test harness for the pure-Rust Mo compiler (`mo-base`, `mo-syntax`, `mo-core`,
`mo-check`, `mo-lower`, `mo-codegen`, `mo-bytecode`, `mo-fe`, and end-to-end `mo-fe` -> `mo-rt`).

Run from the repository root:
    python3 ci/check_rust_frontend.py [--mo PATH] [--mo-fe PATH] [--mo-rt PATH] [--mocore PATH]
                                      [--mo-grammar PATH] [--modb-dir PATH]
                                      [--skip-build] [--require-lean]
                                      [--generated-count N] [--jobs N]

What this checks across Phase 2 (Slices 2A-2E) and Phase 3 Slice 3A:
  1. Workspace lint & safety policy audit (all 14 crates):
       - Root `clippy.toml` configures test exemptions without weakening library lints.
       - Root `Cargo.toml` enforces hardened `writing-good-rust` `[workspace.lints]`
         including `unsafe_code = "forbid"` and `unwrap_used = "deny"`.
       - Every crate under `crates/` opts into `[lints] workspace = true` and puts
         `#![forbid(unsafe_code)]` in the first 5 lines of every entry point (`lib.rs`,
         `main.rs`, `build.rs`).
  2. Lossless `.core` v0.11 canonical round-trip on all committed Lean fixtures (Slice 2A).
  3. Core typechecker differential parity (`mo-fe core-check-batch` vs C & Lean `mocore`).
  4. Generated well-typed Core programs (`ci/gen_core.py`) 3-way check & canonical print parity.
  5. C `mo check --emit=core` front-end seam verification across real Mo programs.
  6. Grammar corpus parity (106/106 `grammar/tests/` vs Bison `mo-grammar`).
  7. Formatter byte-for-byte parity across all tracked `.mo` files + idempotence.
  8. Syntax, lexer, and formatter error diagnostic parity.
  9. Complete `tests/errors/` parity across ALL 235 folders (Slices 2C-3A):
       - Dynamically enumerates EVERY folder in `tests/errors/` with 0 silently skipped.
       - Runs ALL 215 `mo check` cases (including codegen `e1013` and anchor VM `e1405`,
         `e1405-forbids`, plus `e1150`-`e1153` and all generated limit cases) with 0 post-check
         gaps, asserting byte-for-byte text `stderr` vs `expected` and byte-for-byte `--json`
         `stdout` vs C `mo check --json`.
 10. Whole-corpus full `mo-fe check --json` parity vs C `mo check --json` (zero exceptions):
       - All 30 `lib/**/*.mo`, 13 `examples/`, 46 `tests/lower-source/`, 17 `tools/`+`services/`,
         and 238 `tests/golden/` cases verified byte-for-byte against C `mo check --json`.
 11. Pure-Rust lowering (`mo-lower`) verified against `mo-core`, C, and Lean `mocore`.
 12. Pure-Rust codegen (`mo-codegen`) & `.bc` v0.3 writer/disassembler parity (Slice 3A):
       - Deterministic double-compile + verify + `load -> write` byte-exact round-trip across
         all 15 `tests/core/*.core`, all `ci/check_codegen.py` generated suites, `E1201`
         excluded ops, and the full arbitrary-precision BigInt oracle.
       - Byte-for-byte `mo-bytecode` `load -> write` round-trip and disassembler parity against
         C-emitted `.bc` files, plus C `mo run <rust.bc>` cross-verification.
 13. End-to-End 100% Pure-Rust Compilation & Execution Parity (ZERO C in the loop!):
       - `mo-fe emit [--tests]` -> `mo-rt verify` -> `mo-rt anchors` -> `mo-rt run`/`test`
         matched byte-for-byte against golden expectations and C `mo` across all 238 `tests/golden/`,
         46 `tests/lower-source/`, 13 `examples/`, 30 `lib/`, 17 `tools/`+`services/`, and
         (with `--modb-dir`) all 5 MoDB suites (29/29 tests passing!).
 14. `< 0.1 s` `mo-fe check` latency gate (run sequentially alone across all 12 samples).
"""

import argparse
import concurrent.futures
import json
import os
import pathlib
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "ci"))

import check_codegen  # noqa: E402
import check_core_check  # noqa: E402
import check_core_generated  # noqa: E402
import check_lower_source  # noqa: E402
import check_rust_runtime  # noqa: E402
import check_vm  # noqa: E402
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
    assert len(crate_dirs) >= 18, f"expected >= 18 workspace crates, found {len(crate_dirs)}"
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
        bin_dir = crate / "src/bin"
        if bin_dir.is_dir():
            entries.extend(sorted(bin_dir.glob("*.rs")))
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


def find_or_build_mo_fe_and_rt(
    explicit_fe: str | None, explicit_rt: str | None, skip_build: bool
) -> tuple[pathlib.Path, pathlib.Path]:
    bins_to_build: list[str] = []
    if explicit_fe is None:
        bins_to_build.extend(["--bin", "mo-fe"])
    if explicit_rt is None:
        bins_to_build.extend(["--bin", "mo-rt"])
    if not skip_build and bins_to_build:
        subprocess.run(
            ["cargo", "build", "--release", *bins_to_build],
            cwd=ROOT,
            check=True,
        )
    mo_fe = pathlib.Path(explicit_fe).resolve() if explicit_fe else (ROOT / "target/release/mo-fe")
    mo_rt = pathlib.Path(explicit_rt).resolve() if explicit_rt else (ROOT / "target/release/mo-rt")
    assert mo_fe.is_file(), f"mo-fe binary not found: {mo_fe}"
    assert mo_rt.is_file(), f"mo-rt binary not found: {mo_rt}"
    return mo_fe, mo_rt


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


NON_CHECK_COMMAND_ERROR_CASES = frozenset(
    {
        "e1400",
        "e1400-simulation",
        "e1402",
        "e1403",
        "e1404",
        "e1406",
        "e1407",
        "e1408",
        "e1409",
        "e1409-data",
        "e1409-token",
        "e1410",
        "e1411",
        "e1411-function",
        "e1412",
        "e1413",
        "e1418",
        "e1432",
        "e1433",
        "e1439",
    }
)

# All `mo check` error cases (including codegen `e1013` and anchor VM `e1405`, `e1405-forbids`)
# are now handled end-to-end by `mo-fe check` with 0 post-check gaps.
POST_CHECK_ERROR_CASES: frozenset[str] = frozenset()


def _gen_program_too_big(folder: pathlib.Path) -> None:
    _write_error_file(folder, "module Main\n" + "".join(f"use M{i}\n" for i in range(9)))
    line = "a" * 1023 + "\n"
    for i in range(9):
        _write_error_file(folder, f'module M{i}\nbig = """\n' + line * 8190 + '"""\n', f"m{i}.mo")


def _gen_syntax_arena(folder: pathlib.Path) -> None:
    _write_error_file(folder, "module Main\n" + "".join(f"use M{i}\n" for i in range(10)))
    for i in range(10):
        _write_error_file(folder, f"module M{i}\n" + "# padding\n" * 700_000, f"m{i}.mo")


def _gen_too_many_files(folder: pathlib.Path) -> None:
    _write_error_file(folder, "module Main\n" + "".join(f"use M{i}\n" for i in range(4100)))
    for i in range(4100):
        _write_error_file(folder, f"module M{i}\n", f"m{i}.mo")


def _gen_path_too_long(folder: pathlib.Path) -> None:
    _write_error_file(folder, "module Main\nuse " + ".".join(["A" * 200] * 21) + "\n")


def _gen_module_outside_root(folder: pathlib.Path) -> str:
    (folder / "outside.mo").write_text("module Other\n", encoding="utf-8")
    (folder / "project").mkdir()
    (folder / "project/other.mo").symlink_to("../outside.mo")
    _write_error_file(
        folder, "module Main\nuse Other\nfn main()\nend\n", "project/main.mo"
    )
    return "project"


def _gen_checking_arena(folder: pathlib.Path) -> None:
    _write_error_file(
        folder,
        "module Main\nfn main(console: Console)\n" + '  console.print("x")\n' * 150_000 + "end\n",
    )


def _gen_check_work(folder: pathlib.Path) -> None:
    n = 65_000
    _write_error_file(
        folder,
        "module Main\nenum E\n"
        + "".join(f"C{i}\n" for i in range(n))
        + "end\nfn pick(e: E) : Int\n  case e\n"
        + "".join(f"    C{i}: {i}\n" for i in range(n))
        + '  end\nend\nfn main()\n  stop "x"\nend\n',
    )


def _gen_check_type_depth(folder: pathlib.Path) -> None:
    lines = [
        "module Main",
        "fn duplicate(value: T) : (T, T)",
        "(value, value)",
        "end",
        "fn run()",
        "_v0 = 1",
    ]
    lines += [f"_v{i} = duplicate(_v{i - 1})" for i in range(1, 256)]
    _write_error_file(folder, "\n".join(lines + ["end", ""]))


def _gen_check_ability_depth(folder: pathlib.Path) -> None:
    count = 2048
    text = "module Main\n"
    for i in range(count):
        field = f"Option(R{i + 1})" if i + 1 < count else "Option(Int)"
        text += f"struct R{i}\nnext: {field}\nend\n"
    _write_error_file(
        folder,
        text + "fn same(a: Option(R0), b: Option(R0)) : Bool\na == b\nend\nfn main()\nend\n",
    )


def _gen_too_many_registers(folder: pathlib.Path) -> None:
    elements = ",\n".join(str(i) for i in range(70_000))
    _write_error_file(
        folder,
        f"module Main\nfn run() : Int\n  xs = [\n{elements}\n]\n  xs.size\nend\n"
        'fn main()\n  stop "x"\nend\n',
    )


def _gen_lower_arena(folder: pathlib.Path) -> None:
    n = 130_000
    _write_error_file(
        folder,
        "module Main\nfn pick(e: Int) : Int\n  case e\n"
        + "".join(f"    {i}: {i + 1}\n" for i in range(n))
        + '    _: 0\n  end\nend\nfn main()\n  stop "x"\nend\n',
    )


def _gen_lower_work(folder: pathlib.Path) -> None:
    _write_error_file(
        folder,
        'module Main\nfn run() : BigInt\n  ' + "9" * 60_000 + '\nend\nfn main()\n  stop "x"\nend\n',
    )


def _gen_lower_depth(folder: pathlib.Path) -> None:
    n = 70
    text = "module Main\n"
    for i in range(n):
        text += f"fn g{i}(x: T) : Int\n  g{i + 1}([x])\nend\n"
    _write_error_file(
        folder,
        text + f"fn g{n}(_x: T) : Int\n  1\nend\nfn run() : Int\n  g0(1)\nend\n",
    )


ALL_ERROR_GENERATORS = {
    **SYNTAX_ERROR_GENERATORS,
    "program-too-big": _gen_program_too_big,
    "syntax-arena": _gen_syntax_arena,
    "too-many-files": _gen_too_many_files,
    "path-too-long": _gen_path_too_long,
    "module-outside-root": _gen_module_outside_root,
    "checking-arena": _gen_checking_arena,
    "check-work": _gen_check_work,
    "check-type-depth": _gen_check_type_depth,
    "check-ability-depth": _gen_check_ability_depth,
    "too-many-registers": _gen_too_many_registers,
    "lower-arena": _gen_lower_arena,
    "lower-work": _gen_lower_work,
    "lower-depth": _gen_lower_depth,
}


def _mask_library_dependent(text: bytes) -> bytes:
    return re.sub(rb" +", b" ", re.sub(rb"\d+", b"N", text))


def check_all_error_diagnostics_parity(
    mo_bin: pathlib.Path, mo_fe: pathlib.Path, jobs: int
) -> tuple[int, int, int, int]:
    """Dynamically enumerate ALL tests/errors/ folders and verify byte-for-byte text + JSON parity."""
    errors_dir = ROOT / "tests/errors"
    all_dirs = sorted(p for p in errors_dir.iterdir() if p.is_dir())
    assert len(all_dirs) >= 235, f"expected >= 235 tests/errors/ dirs, found {len(all_dirs)}"

    non_check: list[pathlib.Path] = []
    post_check: list[pathlib.Path] = []
    frontend: list[pathlib.Path] = []

    for case in all_dirs:
        assert re.fullmatch(r"e\d{4}(-[a-z0-9-]+)?", case.name), (
            f"unexpected tests/errors entry: {case.name}"
        )
        cmd = (case / "command").read_text().strip() if (case / "command").exists() else "check"
        if cmd != "check":
            assert case.name in NON_CHECK_COMMAND_ERROR_CASES, (
                f"{case.name}: unclassified non-check command {cmd!r}"
            )
            non_check.append(case)
        elif case.name in POST_CHECK_ERROR_CASES:
            post_check.append(case)
        else:
            assert case.name not in NON_CHECK_COMMAND_ERROR_CASES, (
                f"{case.name}: listed as non-check but has command='check'"
            )
            frontend.append(case)

    assert {c.name for c in non_check} == NON_CHECK_COMMAND_ERROR_CASES, (
        f"non-check inventory drifted: {sorted({c.name for c in non_check} ^ NON_CHECK_COMMAND_ERROR_CASES)}"
    )
    assert {c.name for c in post_check} == POST_CHECK_ERROR_CASES, (
        f"post-check inventory drifted: {sorted({c.name for c in post_check} ^ POST_CHECK_ERROR_CASES)}"
    )
    assert len(frontend) + len(post_check) + len(non_check) == len(all_dirs)

    def run_frontend(case: pathlib.Path) -> str:
        arguments = (
            (case / "args").read_text().splitlines() if (case / "args").exists() else ["{main}"]
        )
        exp_err = (case / "expected").read_bytes()
        exp_out = (case / "stdout").read_bytes() if (case / "stdout").exists() else b""
        lib_dep = (case / "library-dependent").exists()
        gen = case / "generated"

        def verify_in(cwd: pathlib.Path, args: list[str]) -> None:
            res = subprocess.run(
                [str(mo_fe), "check", *args],
                cwd=cwd,
                capture_output=True,
                timeout=120,
            )
            assert res.returncode == 1, (
                f"{case.name}: expected rc=1, got {res.returncode} "
                f"(stderr={res.stderr.decode(errors='replace')[:300]})"
            )
            assert res.stdout == exp_out, f"{case.name}: stdout mismatch"
            if lib_dep:
                assert _mask_library_dependent(res.stderr) == _mask_library_dependent(exp_err), (
                    f"{case.name}: masked stderr mismatch\n"
                    f"--- expected\n{exp_err.decode(errors='replace')}\n"
                    f"--- actual\n{res.stderr.decode(errors='replace')}"
                )
            else:
                assert res.stderr == exp_err, (
                    f"{case.name}: stderr mismatch\n"
                    f"--- expected\n{exp_err.decode(errors='replace')}\n"
                    f"--- actual\n{res.stderr.decode(errors='replace')}"
                )

            rj = subprocess.run(
                [str(mo_fe), "check", "--json", *args],
                cwd=cwd,
                capture_output=True,
                timeout=120,
            )
            cj = subprocess.run(
                [str(mo_bin), "check", "--json", *args],
                cwd=cwd,
                capture_output=True,
                timeout=120,
            )
            if cj.returncode == 2:
                assert (rj.returncode, rj.stdout, rj.stderr) == (
                    cj.returncode,
                    cj.stdout,
                    cj.stderr,
                ), (
                    f"{case.name}: JSON refusal mismatch: "
                    f"C=({cj.returncode}, {cj.stderr!r}), Rust=({rj.returncode}, {rj.stderr!r})"
                )
                if any(a.startswith("--emit") for a in args):
                    src_arg = args[-1]
                    out_bc = str(cwd / "_e1013_unused.bc")
                    ej_r = subprocess.run(
                        [str(mo_fe), "emit", "--json", src_arg, "-o", out_bc],
                        cwd=cwd,
                        capture_output=True,
                        timeout=120,
                    )
                    assert ej_r.returncode == 1 and ej_r.stderr == b"", (
                        f"{case.name}: mo-fe emit --json unexpected rc/stderr: {ej_r}"
                    )
                    recs = [json.loads(ln) for ln in ej_r.stdout.splitlines()]
                    codes = [r.get("code") for r in recs if r.get("record") == "diagnostic"]
                    assert codes == [case.name.upper()], (
                        f"{case.name}: mo-fe emit --json codes mismatch: {codes}"
                    )
                return
            assert rj.returncode == cj.returncode == 1 and rj.stderr == cj.stderr == b"", (
                f"{case.name}: JSON rc/stderr mismatch: "
                f"C=({cj.returncode}, {cj.stderr!r}), Rust=({rj.returncode}, {rj.stderr!r})"
            )
            if lib_dep:
                r_objs = [json.loads(ln) for ln in rj.stdout.splitlines()]
                c_objs = [json.loads(ln) for ln in cj.stdout.splitlines()]
                assert len(r_objs) == len(c_objs) and len(r_objs) >= 3, (
                    f"{case.name}: JSON record count mismatch: C={len(c_objs)}, Rust={len(r_objs)}"
                )
                for r_obj, c_obj in zip(r_objs, c_objs):
                    assert (
                        r_obj.get("record") == c_obj.get("record")
                        and r_obj.get("code") == c_obj.get("code")
                        and r_obj.get("headline") == c_obj.get("headline")
                    ), f"{case.name}: JSON record mismatch: C={c_obj!r}, Rust={r_obj!r}"
            else:
                assert rj.stdout == cj.stdout, (
                    f"{case.name}: byte-for-byte JSON stdout mismatch\n"
                    f"--- C\n{cj.stdout.decode(errors='replace')}\n"
                    f"--- Rust\n{rj.stdout.decode(errors='replace')}"
                )

        if gen.exists():
            gen_name = gen.read_text().strip()
            assert gen_name in ALL_ERROR_GENERATORS, (
                f"{case.name}: missing generator {gen_name!r} in ALL_ERROR_GENERATORS"
            )
            with tempfile.TemporaryDirectory(prefix="mo-rust-2d-err-") as folder:
                where = ALL_ERROR_GENERATORS[gen_name](pathlib.Path(folder))
                cwd = pathlib.Path(folder) / (where or ".")
                args = [a.replace("{main}", "main.mo") for a in arguments]
                verify_in(cwd, args)
        else:
            main = str((case / "main.mo").relative_to(ROOT))
            args = [a.replace("{main}", main) for a in arguments]
            verify_in(ROOT, args)
        return case.name

    def run_post_check(case: pathlib.Path) -> str:
        # Every post-check case must pass front-end typechecking cleanly (rc=0) in mo-fe check.
        gen = case / "generated"
        if gen.exists():
            gen_name = gen.read_text().strip()
            assert gen_name in ALL_ERROR_GENERATORS, (
                f"{case.name}: missing generator {gen_name!r} in ALL_ERROR_GENERATORS"
            )
            with tempfile.TemporaryDirectory(prefix="mo-rust-2d-post-") as folder:
                where = ALL_ERROR_GENERATORS[gen_name](pathlib.Path(folder))
                cwd = pathlib.Path(folder) / (where or ".")
                res = subprocess.run(
                    [str(mo_fe), "check", "main.mo"],
                    cwd=cwd,
                    capture_output=True,
                    timeout=120,
                )
        else:
            main = str((case / "main.mo").relative_to(ROOT))
            res = subprocess.run(
                [str(mo_fe), "check", main],
                cwd=ROOT,
                capture_output=True,
                timeout=120,
            )
        assert res.returncode == 0 and res.stderr == b"", (
            f"{case.name}: post-check case should pass front-end checking, got rc={res.returncode}: "
            f"{res.stderr.decode(errors='replace')}"
        )
        return case.name

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        checked_front = list(pool.map(run_frontend, frontend))
        checked_post = list(pool.map(run_post_check, post_check))
    return len(all_dirs), len(checked_front), len(checked_post), len(non_check)


def check_whole_corpus_full_parity(
    mo_bin: pathlib.Path, mo_fe: pathlib.Path, jobs: int
) -> tuple[int, int, int, int, int]:
    """Verify full `mo-fe check --json` byte-for-byte parity vs C `mo check --json` across all corpora."""
    # 1. All 30 lib/**/*.mo
    lib_files = sorted(str(p.relative_to(ROOT)) for p in (ROOT / "lib").rglob("*.mo"))
    assert len(lib_files) == 30, f"expected 30 lib/**/*.mo files, found {len(lib_files)}"

    # 2. All 13 examples/
    example_files: list[str] = []
    for p in sorted((ROOT / "examples").glob("*.mo")):
        example_files.append(str(p.relative_to(ROOT)))
    for d in sorted(p for p in (ROOT / "examples").iterdir() if p.is_dir()):
        main_mo = d / "main.mo"
        if main_mo.is_file():
            example_files.append(str(main_mo.relative_to(ROOT)))
    assert len(example_files) == 13, f"expected 13 examples/, found {len(example_files)}"

    # 3. All 46 tests/lower-source/*/main.mo
    lower_files = sorted(
        str((d / "main.mo").relative_to(ROOT))
        for d in (ROOT / "tests/lower-source").iterdir()
        if d.is_dir() and (d / "main.mo").is_file()
    )
    assert len(lower_files) == 46, f"expected 46 tests/lower-source/*/main.mo, found {len(lower_files)}"

    # 4. All 17 tools/ + services/ entry points
    tool_service_files: list[str] = []
    for parent in ("tools", "services"):
        base = ROOT / parent
        for p in sorted(base.glob("*.mo")):
            tool_service_files.append(str(p.relative_to(ROOT)))
        for d in sorted(p for p in base.iterdir() if p.is_dir()):
            if (d / "main.mo").is_file():
                tool_service_files.append(str((d / "main.mo").relative_to(ROOT)))
            if (d / "queue-tests.mo").is_file():
                tool_service_files.append(str((d / "queue-tests.mo").relative_to(ROOT)))
    assert len(tool_service_files) >= 17, (
        f"expected >= 17 tools/+services/ entry points, found {len(tool_service_files)}"
    )

    # 5. All 238 tests/golden/ directories
    golden_dirs = sorted(p for p in (ROOT / "tests/golden").iterdir() if p.is_dir())
    assert len(golden_dirs) == 238, f"expected 238 tests/golden/ dirs, found {len(golden_dirs)}"
    golden_files: list[str] = []
    for d in golden_dirs:
        entry = (d / "entry").read_text().strip() if (d / "entry").exists() else None
        golden_files.append(entry or str((d / "main.mo").relative_to(ROOT)))

    all_files = lib_files + example_files + lower_files + tool_service_files + golden_files

    def verify_one(rel: str) -> tuple[str, int]:
        rj = subprocess.run(
            [str(mo_fe), "check", "--json", rel],
            cwd=ROOT,
            capture_output=True,
            timeout=60,
        )
        cj = subprocess.run(
            [str(mo_bin), "check", "--json", rel],
            cwd=ROOT,
            capture_output=True,
            timeout=60,
        )
        if rj.returncode == cj.returncode and rj.stdout == cj.stdout and rj.stderr == cj.stderr:
            return rel, rj.returncode
        raise AssertionError(
            f"full mo-fe check --json mismatch on {rel}:\n"
            f"C rc={cj.returncode}, stderr={cj.stderr.decode(errors='replace')}\n"
            f"--- C stdout\n{cj.stdout.decode(errors='replace')}\n"
            f"Rust rc={rj.returncode}, stderr={rj.stderr.decode(errors='replace')}\n"
            f"--- Rust stdout\n{rj.stdout.decode(errors='replace')}"
        )

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        outcomes = list(pool.map(verify_one, all_files))

    # Also verify `mo-fe check-batch` (default full stage) agrees with `mo-fe check` across all files.
    batch_verdicts = run_batch_lines([str(mo_fe), "check-batch", "--"], all_files, jobs=jobs)
    assert len(batch_verdicts) == len(outcomes)
    for (rel, expected_rc), verdict in zip(outcomes, batch_verdicts):
        got_rc = 0 if verdict == "0" else 1
        assert got_rc == expected_rc, (
            f"mo-fe check-batch disagreed with mo-fe check on {rel}: rc={expected_rc}, batch={verdict!r}"
        )

    return (
        len(lib_files),
        len(example_files),
        len(lower_files),
        len(tool_service_files),
        len(golden_files),
    )


def _wrap_program_as_run(raw: bytes, label: str) -> bytes:
    prefix = f"(core {check_core_check.VERSION} ".encode()
    assert raw.startswith(f"(core {check_core_check.VERSION} (Program ".encode()), (
        f"unexpected lowered Core envelope on {label}: {raw[:120]!r}"
    )
    inner = raw[len(prefix) :].rstrip()
    assert inner.endswith(b")"), f"malformed Core envelope on {label}"
    return check_core_check.run(inner[:-1].decode("utf-8"))


def _lean_oracle_eval(
    mocore: pathlib.Path,
    path: pathlib.Path,
    world: pathlib.Path,
    test: str | None = None,
) -> list[str]:
    args = [str(mocore), str(path), str(world), "--fuel", "10000"]
    if test is not None:
        args += ["--test", test]
    res = subprocess.run(args, capture_output=True, text=True, timeout=30)
    assert res.returncode in (0, 1, 2), (path, res.returncode, res.stdout, res.stderr)
    return check_vm.outcome_of(res.stdout)


def run_lower_batch(
    mo_fe: pathlib.Path, files: list[str], jobs: int, tests_mode: bool = False
) -> list[tuple[bool, bytes | int]]:
    batches = [files[start : start + BATCH] for start in range(0, len(files), BATCH)]
    prefix = [str(mo_fe), "lower-batch", *(["--tests"] if tests_mode else []), "--"]

    def one(batch: list[str]) -> list[tuple[bool, bytes | int]]:
        res = subprocess.run(
            [*prefix, *batch],
            cwd=ROOT,
            capture_output=True,
            check=True,
            timeout=180,
        )
        return decode_format_batch(res.stdout)

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        out: list[tuple[bool, bytes | int]] = []
        for items in pool.map(one, batches):
            out.extend(items)
    return out


def check_rust_lowering_and_lean_parity(
    mo_bin: pathlib.Path,
    mo_fe: pathlib.Path,
    c_check: pathlib.Path,
    mocore: pathlib.Path | None,
    jobs: int,
) -> tuple[int, int, int, int]:
    """Slice 2E: verify Rust mo-lower across all corpora + Lean mocore typechecking & evaluation."""
    # 1. Collect all non-compile-error real `.mo` entry points across lib/, examples/,
    #    tests/lower-source/, tools/, services/, and tests/golden/.
    corpus: list[tuple[str, str]] = []
    for p in sorted((ROOT / "lib").rglob("*.mo")):
        corpus.append((f"lib/{p.relative_to(ROOT / 'lib')}", str(p.relative_to(ROOT))))
    for p in sorted((ROOT / "examples").glob("*.mo")):
        corpus.append((f"example/{p.stem}", str(p.relative_to(ROOT))))
    for d in sorted(p for p in (ROOT / "examples").iterdir() if p.is_dir()):
        if (d / "main.mo").is_file():
            corpus.append((f"example/{d.name}", str((d / "main.mo").relative_to(ROOT))))
    for d in sorted(p for p in (ROOT / "tests/lower-source").iterdir() if p.is_dir()):
        if (d / "main.mo").is_file():
            corpus.append((f"lower-source/{d.name}", str((d / "main.mo").relative_to(ROOT))))
    for parent in ("tools", "services"):
        base = ROOT / parent
        for p in sorted(base.glob("*.mo")):
            corpus.append((f"{parent}/{p.stem}", str(p.relative_to(ROOT))))
        for d in sorted(p for p in base.iterdir() if p.is_dir()):
            if (d / "main.mo").is_file():
                corpus.append((f"{parent}/{d.name}", str((d / "main.mo").relative_to(ROOT))))
            if (d / "queue-tests.mo").is_file():
                corpus.append(
                    (f"{parent}/{d.name}/queue-tests", str((d / "queue-tests.mo").relative_to(ROOT)))
                )
    for d in sorted(p for p in (ROOT / "tests/golden").iterdir() if p.is_dir()):
        exp_stdout = (d / "stdout").read_bytes() if (d / "stdout").exists() else b""
        exp_stderr = (d / "stderr").read_bytes() if (d / "stderr").exists() else b""
        exp_status = int((d / "status").read_text().strip()) if (d / "status").exists() else 0
        if exp_status != 0 and exp_stdout == b"" and exp_stderr.startswith(b"error E"):
            continue
        entry = (d / "entry").read_text().strip() if (d / "entry").exists() else None
        rel = entry or str((d / "main.mo").relative_to(ROOT))
        if (ROOT / rel).is_file():
            corpus.append((f"golden/{d.name}", rel))

    assert len(corpus) >= 330, f"expected >= 330 lowerable .mo programs, found {len(corpus)}"

    # Verify `mo-fe check --emit=core` CLI path directly on representative fixtures.
    for sample in (
        "examples/greet.mo",
        "tests/lower-source/json-ability/main.mo",
        "tests/lower-source/api-server/main.mo",
    ):
        res = subprocess.run(
            [str(mo_fe), "check", "--emit=core", sample],
            cwd=ROOT,
            capture_output=True,
            timeout=60,
        )
        assert res.returncode == 0 and res.stderr == b"", (
            f"mo-fe check --emit=core failed on {sample}: {res.stderr.decode(errors='replace')}"
        )
        assert res.stdout.startswith(f"(core {check_core_check.VERSION} (Program ".encode()), (
            f"unexpected check --emit=core output on {sample}"
        )

    rel_paths = [rel for _, rel in corpus]
    lowered_results = run_lower_batch(mo_fe, rel_paths, jobs=jobs, tests_mode=False)
    assert len(lowered_results) == len(corpus)

    lean_typechecked = 0
    with tempfile.TemporaryDirectory(prefix="mo-rust-2e-lower-") as tmp:
        tmp_path = pathlib.Path(tmp)
        prog_files: list[str] = []
        run_files: list[str] = []
        raw_docs: list[bytes] = []
        for idx, ((label, rel), (ok, payload)) in enumerate(zip(corpus, lowered_results)):
            assert ok and isinstance(payload, bytes), (
                f"mo-fe lower-batch failed on {label} ({rel}): code={payload!r}"
            )
            pf = tmp_path / f"prog_{idx}.core"
            pf.write_bytes(payload)
            rf = tmp_path / f"run_{idx}.core"
            rf.write_bytes(_wrap_program_as_run(payload, label))
            prog_files.append(str(pf))
            run_files.append(str(rf))
            raw_docs.append(payload)

        # Canonical round-trip check on every Rust-lowered Core Program.
        printed = run_batch_printed(
            [str(mo_fe), "core-print-batch", "--kind", "program"], prog_files, jobs
        )
        assert len(printed) == len(corpus)
        for (label, _), raw, got in zip(corpus, raw_docs, printed):
            assert got.encode("utf-8") == raw, f"canonical round-trip diff on Rust-lowered {label}"

        # 2-way Rust mo-core == C test_core_check verification on every Rust-lowered Program.
        rs_verdicts = run_batch_lines(
            [str(mo_fe), "core-check-batch", "--kind", "program"], prog_files, jobs
        )
        c_verdicts = run_batch_lines([str(c_check)], run_files, jobs)
        standalone_runs: list[tuple[str, str]] = []
        for (label, _), raw, rf, rv, cv in zip(corpus, raw_docs, run_files, rs_verdicts, c_verdicts):
            rs_parts = rv.split()
            c_parts = cv.split()
            assert rs_parts[0] == c_parts[0], (
                f"{label}: Core verdict mismatch on Rust-lowered output: C={cv!r}, mo-fe={rv!r}"
            )
            if rs_parts[0] == "0":
                standalone_runs.append((label, rf))
            else:
                assert b"(sim (start " in raw or b"(sim (send " in raw, (
                    f"{label}: unexpected standalone Core refusal on Rust-lowered output: {rv!r}"
                )
                assert rs_parts[1] == c_parts[1] == "E1037", (
                    f"{label}: expected E1037 on service-dependent Core, got C={cv!r}, mo-fe={rv!r}"
                )

        # Lean `mocore` typechecking across every standalone-checkable Rust-lowered Program.
        if mocore is not None:
            def check_lean_one(item: tuple[str, str]) -> None:
                label, rf = item
                res = subprocess.run([str(mocore), rf], capture_output=True, timeout=30)
                assert res.returncode in (0, 1, 2, 3, 6), (
                    f"{label}: Lean mocore refused Rust-lowered Core (rc={res.returncode}): "
                    f"{res.stdout.decode(errors='replace')} {res.stderr.decode(errors='replace')}"
                )

            with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
                list(pool.map(check_lean_one, standalone_runs))
            lean_typechecked = len(standalone_runs)

        # 2. Semantic evaluation & metadata parity on ALL check_lower_source.py suites:
        #    CASES (107), SPEC_CASES (26 / 76 targets), MODULE_CASES (21 / 27 targets), REFUSALS (3).
        world = tmp_path / "world.core"
        world.write_text(f"(core {check_core_check.VERSION} {check_core_check.WORLD})\n")
        eval_matched = 0
        c_eval_matched = 0

        # 2a. REFUSALS (all 3 must refuse with E1150)
        for name, body in check_lower_source.REFUSALS.items():
            ref_dir = tmp_path / f"refusal_{name}"
            ref_dir.mkdir()
            src = ref_dir / "main.mo"
            src.write_text("module Main\n" + body + "\n")
            res = subprocess.run(
                [str(mo_fe), "lower", "--lib", str(ROOT / "lib"), str(src)],
                capture_output=True,
                text=True,
                timeout=30,
            )
            assert res.returncode == 1 and res.stdout == "" and "E1150" in res.stderr, (
                f"refusal {name}: expected E1150, got rc={res.returncode} stderr={res.stderr!r}"
            )

        # 2b. CASES (107 pure/fixture cases + linear scaling check + C comparison on pure fixtures)
        lengths: dict[str, int] = {}

        def eval_case(item: tuple[str, tuple[str, object]]) -> tuple[str, int, int, int]:
            name, (body, status) = item
            cdir = tmp_path / f"case_{name}"
            cdir.mkdir()
            src = cdir / "main.mo"
            src.write_text("module Main\n" + body + "\n")
            res = subprocess.run(
                [str(mo_fe), "lower", "--lib", str(ROOT / "lib"), str(src)],
                capture_output=True,
                text=True,
                timeout=30,
            )
            assert res.returncode == 0, f"case {name} failed mo-fe lower: {res.stderr}"
            core_path = cdir / "lower.core"
            core_path.write_text(res.stdout)
            meta = check_core_check.parse(res.stdout)[2][-1]
            assert meta[0] == "Metadata", (name, "missing Metadata")
            cnames = [cl[1] for fn in meta[3] for cl in fn[4][4]]
            assert len(cnames) == len(set(cnames)), (name, "duplicate closure names", cnames)
            expected = ["returned", str(status)] if isinstance(status, int) else status
            em = cm = 0
            if mocore is not None:
                got = _lean_oracle_eval(mocore, core_path, world)
                assert got == expected, f"case {name}: Lean want={expected}, got={got}"
                em = 1
                # Cross-check against C `mo check --emit=core` on all pure tests/lower-source fixtures.
                if (ROOT / "tests/lower-source" / name / "main.mo").is_file():
                    c_res = subprocess.run(
                        [str(mo_bin), "check", "--emit=core", "--lib", str(ROOT / "lib"), str(src)],
                        capture_output=True,
                        text=True,
                        timeout=30,
                    )
                    assert c_res.returncode == 0, f"C emit=core failed on {name}: {c_res.stderr}"
                    c_core = cdir / "c.core"
                    c_core.write_text(c_res.stdout)
                    c_got = _lean_oracle_eval(mocore, c_core, world)
                    assert got == c_got, f"case {name}: Rust={got} != C={c_got}"
                    cm = 1
            return name, len(res.stdout.encode("utf-8")), em, cm

        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            for name, size, em, cm in pool.map(eval_case, check_lower_source.CASES.items()):
                lengths[name] = size
                eval_matched += em
                c_eval_matched += cm

        for family in ("bool", "pattern"):
            sizes = [lengths[f"linear_{family}_{n}"] for n in (4, 8, 16)]
            assert all(b <= 3 * a for a, b in zip(sizes, sizes[1:])), (
                f"non-linear lowering blowup in {family}: {sizes}"
            )

        # 2c. SPEC_CASES (26 programs / 76 targets + spec metadata invariants)
        def eval_spec(item: tuple[str, tuple[str, dict[str | None, list[str]]]]) -> int:
            name, (body, selections) = item
            sdir = tmp_path / f"spec_{name}"
            sdir.mkdir()
            src = sdir / "main.mo"
            original = ("module Main\n" + body + "\n").encode("utf-8")
            src.write_bytes(original)
            res = subprocess.run(
                [str(mo_fe), "lower", "--tests", "--lib", str(ROOT / "lib"), str(src)],
                capture_output=True,
                text=True,
                timeout=30,
            )
            assert res.returncode == 0, f"spec {name} failed mo-fe lower --tests: {res.stderr}"
            core_path = sdir / "spec.core"
            core_path.write_text(res.stdout)
            prog = check_core_check.parse(res.stdout)[2]
            meta = prog[-1]
            assert meta[0] == "Metadata", name
            fnames = [fn[1] for fn in prog[2]]
            assert len(fnames) == len(set(fnames)), (name, "duplicate spec names")
            matched = 0
            if mocore is not None:
                for target, expected in selections.items():
                    actual_target, oracle_path = target, core_path
                    if name == "service-descriptor" and target is not None:
                        probe = check_core_check.parse(res.stdout)
                        payload = probe[2]
                        actual_target = "%lower.service.value"
                        prim = "(int (toText))" if target.endswith(".start") else "(time (durToText))"
                        payload[2].append(
                            check_core_check.parse(
                                f'(FunDecl {check_core_check.wire(actual_target)} () (unit) '
                                f'(fail (prim {prim} ((call {check_core_check.wire(target)} ())))) '
                                f'"Main" (test) {len(payload[3])})'
                            )
                        )
                        payload[5] = check_core_check.parse("(noSource)")
                        oracle_path = sdir / f"probe_{target}.core"
                        oracle_path.write_text(check_core_check.wire(probe) + "\n")
                    anchor = target is not None and any(
                        f[1] == target and f[6][0] == "anchor" for f in prog[2]
                    )
                    if anchor:
                        probe = check_core_check.parse(res.stdout)
                        payload = probe[2]
                        actual_target = "%lower.source.anchor.answer"
                        payload[2].append(
                            check_core_check.parse(
                                f'(FunDecl {check_core_check.wire(actual_target)} () (unit) '
                                f'(fail (prim (int (toText)) ((call {check_core_check.wire(target)} ())))) '
                                f'"Main" (test) {len(payload[3])})'
                            )
                        )
                        payload[5] = check_core_check.parse("(noSource)")
                        oracle_path = sdir / f"anchor_{target}.core"
                        oracle_path.write_text(check_core_check.wire(probe) + "\n")
                    got = _lean_oracle_eval(mocore, oracle_path, world, actual_target)
                    if anchor and got[0] == "failed":
                        got[0] = "returned"
                    assert got == expected, f"spec {name}/{target}: want={expected}, got={got}"
                    matched += 1
            return matched

        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            eval_matched += sum(pool.map(eval_spec, check_lower_source.SPEC_CASES.items()))

        # 2d. MODULE_CASES (21 multi-module programs / 27 targets + owner/export/span checks)
        def eval_module(item: tuple[str, tuple[dict[str, str], dict[str | None, list[str]]]]) -> int:
            name, (files, selections) = item
            mdir = tmp_path / f"mod_{name}"
            mdir.mkdir()
            for fn, text in files.items():
                (mdir / fn).write_text(text)
            res = subprocess.run(
                [str(mo_fe), "lower", "--tests", "--no-library", str(mdir / "main.mo")],
                capture_output=True,
                text=True,
                timeout=30,
            )
            assert res.returncode == 0, f"module {name} failed mo-fe lower: {res.stderr}"
            core_path = mdir / "lower.core"
            core_path.write_text(res.stdout)
            prog = check_core_check.parse(res.stdout)[2]
            meta = prog[-1]
            assert meta[0] == "Metadata", name
            assert {pathlib.Path(p).name for p in meta[1]} == set(files), (name, meta[1])
            matched = 0
            if mocore is not None:
                for target, expected in selections.items():
                    got = _lean_oracle_eval(mocore, core_path, world, target)
                    assert got == expected, f"module {name}/{target}: want={expected}, got={got}"
                    matched += 1
            return matched

        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            eval_matched += sum(pool.map(eval_module, check_lower_source.MODULE_CASES.items()))

    return len(corpus), lean_typechecked, eval_matched, c_eval_matched


def check_modb_full_parity(
    mo_bin: pathlib.Path,
    mo_fe: pathlib.Path,
    mocore: pathlib.Path | None,
    modb_dir: pathlib.Path,
    jobs: int,
) -> tuple[int, int]:
    """Verify full `mo-fe check --json` parity AND `mo-fe lower` + Lean typecheck across MoDB."""
    root = modb_dir.resolve()
    assert root.is_dir(), f"--modb-dir is not a directory: {root}"
    files = sorted(root.rglob("*.mo"))
    assert len(files) >= 5, f"expected >= 5 .mo files in {root}, found {len(files)}"

    with tempfile.TemporaryDirectory(prefix="mo-rust-modb-2e-") as tmp:
        tmp_path = pathlib.Path(tmp)

        def verify_one(item: tuple[int, pathlib.Path]) -> tuple[str, int]:
            idx, path = item
            rel = str(path.relative_to(root))
            rj = subprocess.run(
                [str(mo_fe), "check", "--json", rel],
                cwd=root,
                capture_output=True,
                timeout=60,
            )
            cj = subprocess.run(
                [str(mo_bin), "check", "--json", rel],
                cwd=root,
                capture_output=True,
                timeout=60,
            )
            assert rj.returncode == cj.returncode and rj.stdout == cj.stdout and rj.stderr == cj.stderr, (
                f"MoDB {rel}: mo-fe check --json mismatch\n"
                f"C rc={cj.returncode}: {cj.stdout.decode(errors='replace')}\n"
                f"Rust rc={rj.returncode}: {rj.stdout.decode(errors='replace')}"
            )
            lean_ok = 0
            if rj.returncode == 0:
                lr = subprocess.run(
                    [str(mo_fe), "lower", rel],
                    cwd=root,
                    capture_output=True,
                    timeout=60,
                )
                assert lr.returncode == 0 and lr.stderr == b"", (
                    f"MoDB {rel}: mo-fe lower failed: {lr.stderr.decode(errors='replace')}"
                )
                if mocore is not None and b"(sim (start " not in lr.stdout and b"(sim (send " not in lr.stdout:
                    rf = tmp_path / f"modb_{idx}.core"
                    rf.write_bytes(_wrap_program_as_run(lr.stdout, f"MoDB/{rel}"))
                    mc = subprocess.run([str(mocore), str(rf)], capture_output=True, timeout=30)
                    assert mc.returncode in (0, 1, 2, 3, 6), (
                        f"MoDB {rel}: Lean mocore refused lowered Core (rc={mc.returncode}): "
                        f"{mc.stdout.decode(errors='replace')} {mc.stderr.decode(errors='replace')}"
                    )
                    lean_ok = 1
            return rel, lean_ok

        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            results = list(pool.map(verify_one, enumerate(files)))
    return len(results), sum(ok for _, ok in results)


def check_rust_codegen_and_bytecode_parity(
    mo_bin: pathlib.Path,
    mo_fe: pathlib.Path,
    mo_rt: pathlib.Path,
    jobs: int,
) -> tuple[int, int, int, int, int]:
    """Slice 3A: codegen determinism, verifier parity, BigInt oracle, and `.bc` round-trip/disasm."""
    version = check_core_check.VERSION
    with tempfile.TemporaryDirectory(prefix="mo-rust-3a-codegen-") as tmp:
        tmp_path = pathlib.Path(tmp)

        # 1. All 15 committed tests/core/*.core fixtures: double-compile deterministic + verified + round-trip.
        fixtures = sorted((ROOT / "tests/core").glob("*.core"))
        assert len(fixtures) == 15, f"expected 15 tests/core/*.core fixtures, found {len(fixtures)}"
        fix_bc_dir = tmp_path / "fixtures_bc"
        fix_bc_dir.mkdir()
        fix_lines = run_batch_lines(
            [str(mo_fe), "codegen-core-batch", "--kind", "run", "--out-dir", str(fix_bc_dir), "--"],
            [str(p) for p in fixtures],
            jobs=1,
        )
        assert len(fix_lines) == len(fixtures)
        for p, line in zip(fixtures, fix_lines):
            assert line.startswith("0 "), f"codegen failed on {p.name}: {line!r}"
        for idx in range(len(fixtures)):
            bc_path = fix_bc_dir / f"{idx}.bc"
            ver = subprocess.run([str(mo_rt), "verify", str(bc_path)], capture_output=True, timeout=30)
            assert ver.returncode == 0 and ver.stderr == b"", (
                f"mo-rt verify failed on fixture {fixtures[idx].name}: {ver.stderr.decode(errors='replace')}"
            )

        # 2. All ci/check_codegen.py generated suites + operation signatures + E1201 exclusions.
        generated: list[str] = []
        excluded: list[str] = []
        gen_dir = tmp_path / "gen"
        gen_dir.mkdir()
        for i in range(256):
            body = check_codegen.expression(random.Random(207 + i), i % 4)
            entry = "start" if i % 2 else "main"
            payload = (
                f'(Program () ('
                f'(FunDecl "{entry}" (("b" (bool)) ("x" (int))) (int) {body} "Generated" (function) 0) '
                f'(FunDecl "identity" (("x" (int))) (int) (var "x") "Generated" (function) 0)'
                f') () "{entry}" (noSource))'
            )
            p = gen_dir / f"expr-{i}.core"
            p.write_text(f"(core {version} {payload})\n", encoding="utf-8")
            generated.append(str(p))
        for i in range(256):
            body = check_codegen.reference_tail(random.Random(540 + i), i % 4)
            entry = "reference_entry" if i % 2 else "main"
            payload = (
                f'(Program ((DataDecl "Pair" ((Ctor "Pair" (("a" (text)) ("b" (text))))))) ('
                f'(FunDecl "{entry}" (("b" (bool)) ("x" (text))) (text) {body} "Generated" (function) 0) '
                f'(FunDecl "identity" (("x" (text))) (text) (var "x") "Generated" (function) 0)'
                f') () "{entry}" (noSource))'
            )
            p = gen_dir / f"ref-{i}.core"
            p.write_text(f"(core {version} {payload})\n", encoding="utf-8")
            generated.append(str(p))
        for i in range(192):
            p = gen_dir / f"globals-{i}.core"
            p.write_text(f"(core {version} {check_codegen.global_program(i)})\n", encoding="utf-8")
            generated.append(str(p))
        for i in range(384):
            p = gen_dir / f"closures-{i}.core"
            p.write_text(f"(core {version} {check_codegen.closure_program(i)})\n", encoding="utf-8")
            generated.append(str(p))
        for i in range(192):
            p = gen_dir / f"joins-{i}.core"
            p.write_text(f"(core {version} {check_codegen.join_program(i)})\n", encoding="utf-8")
            generated.append(str(p))
        for i in range(132):
            p = gen_dir / f"expansions-{i}.core"
            p.write_text(f"(core {version} {check_codegen.expansion_program(i)})\n", encoding="utf-8")
            generated.append(str(p))
        for i, (prim, types, ret, spec) in enumerate(check_core_check.signatures()):
            params = [(f"arg{j}", t) for j, t in enumerate(types)]
            args_sexp = " ".join(f'(var "arg{j}")' for j in range(len(types)))
            body = f"(prim {prim} ({args_sexp}))"
            if spec:
                body = f'(let_ "discard" {body} (lit (int 0)))'
                params.append(("result", "(int)"))
            payload = check_core_check.program(
                body, "(int)" if spec else ret, params, "promise" if spec else "function"
            )
            p = gen_dir / f"op-{i}.core"
            p.write_text(f"(core {version} {payload})\n", encoding="utf-8")
            if prim.startswith(("(eq (fn ", "(eq (cap ", "(eq (data ", "(structural (data ")):
                excluded.append(str(p))
            else:
                generated.append(str(p))

        gen_lines = run_batch_lines(
            [str(mo_fe), "codegen-core-batch", "--kind", "program", "--"], generated, jobs
        )
        assert len(gen_lines) == len(generated)
        for path, line in zip(generated, gen_lines):
            assert line.startswith("0 "), f"codegen failed on generated {path}: {line!r}"

        exc_lines = run_batch_lines(
            [str(mo_fe), "codegen-core-batch", "--kind", "program", "--"], excluded, jobs
        )
        assert len(exc_lines) == len(excluded)
        for path, line in zip(excluded, exc_lines):
            assert line.startswith("5 E1201"), (
                f"expected E1201 on lowering-impossible {path}, got {line!r}"
            )

        # 3. Arbitrary-precision BigInt oracle (matching ci/check_codegen.py:bigint_oracle).
        sys.set_int_max_str_digits(0)
        rng = random.Random(1202)
        numbers = [0, 1, -1]
        for bits in [8, 16, 31, 32, 33, 63, 64, 65, 127, 128, 129, 511, 512, 513, 4096]:
            numbers.extend((1 << bits) + delta for delta in (-1, 0, 1))
        for groups in [1, 2, 4, 8, 16, 32, 64, 128, 512, 4096]:
            for digits in [9 * groups - 1, 9 * groups, 9 * groups + 1]:
                numbers.extend([10 ** (digits - 1) + 17, -(10**digits - 1)])
        numbers.extend(
            rng.getrandbits(rng.randrange(1, 32769)) * rng.choice([-1, 1]) for _ in range(96)
        )
        bigint_dir = tmp_path / "bigints"
        bigint_dir.mkdir()
        bi_paths: list[str] = []
        bi_expected: list[str] = []
        for i, number in enumerate(numbers):
            p = bigint_dir / f"bigint-{i}.core"
            p.write_text(
                f"(core {version} {check_core_check.program(f'(lit (bigInt {number}))', '(bigInt)')})\n",
                encoding="utf-8",
            )
            bi_paths.append(str(p))
            magnitude = abs(number).to_bytes((abs(number).bit_length() + 7) // 8, "little")
            sign = 0 if number == 0 else 1 if number > 0 else 2
            bi_expected.append(f"0 {sign}:{magnitude.hex()}")
        bi_lines = run_batch_lines(
            [str(mo_fe), "codegen-core-batch", "--kind", "program", "--bigints", "--"],
            bi_paths,
            jobs,
        )
        assert bi_lines == bi_expected, "compiled BigInt signs/magnitudes differ from Python oracle"

        # 4. C-emitted `.bc` byte-for-byte writer round-trip + disassembler parity + C cross-run.
        rt_programs = sorted(
            [*(ROOT / "examples").glob("*.mo"), *(ROOT / "examples").glob("*/main.mo")]
        ) + [
            ROOT / "tests/lower-source/service-descriptor/main.mo",
            ROOT / "tests/lower-source/service-simulation/main.mo",
            ROOT / "tests/lower-source/json-ability/main.mo",
            ROOT / "services/task-queue/queue-tests.mo",
        ]
        bc_dir = tmp_path / "c_bc"
        bc_dir.mkdir()

        def verify_bc_one(item: tuple[int, pathlib.Path]) -> None:
            idx, prog = item
            rel = str(prog.relative_to(ROOT))
            c_bc = bc_dir / f"c_{idx}.bc"
            rs_bc = bc_dir / f"rs_{idx}.bc"
            src_text = prog.read_text(encoding="utf-8")
            has_entry = bool(re.search(r"^(?:fn main\b|service\b|api\b)", src_text, re.M))
            emit_flags = [] if has_entry else ["--tests"]
            c_emit = subprocess.run(
                [str(mo_bin), "emit", *emit_flags, rel, "-o", str(c_bc)],
                cwd=ROOT,
                capture_output=True,
                timeout=60,
            )
            assert c_emit.returncode == 0, (
                f"C emit failed on {rel}: {c_emit.stderr.decode(errors='replace')}"
            )
            # Rust `load -> write` reproduces C's exact `.bc` bytes byte-for-byte.
            rt_res = subprocess.run(
                [str(mo_fe), "roundtrip-bc", str(c_bc)],
                cwd=ROOT,
                capture_output=True,
                timeout=60,
            )
            assert rt_res.returncode == 0 and rt_res.stderr == b"", (
                f"mo-fe roundtrip-bc failed on C-emitted {rel}: {rt_res.stderr.decode(errors='replace')}"
            )
            # Check `--emit=bytecode` disassembler parity on non-test builds (same .bc bytes).
            if has_entry:
                c_dis = subprocess.run(
                    [str(mo_bin), "check", "--emit=bytecode", rel],
                    cwd=ROOT,
                    capture_output=True,
                    timeout=60,
                )
                assert c_dis.returncode == 0 and c_dis.stderr == b"", (
                    f"C check --emit=bytecode failed on {rel}: {c_dis.stderr.decode(errors='replace')}"
                )
                assert rt_res.stdout == c_dis.stdout, (
                    f"disassembler mismatch vs C on C-emitted {rel} "
                    f"(C={len(c_dis.stdout)}B, Rust={len(rt_res.stdout)}B)"
                )
            # Rust `mo-fe check --emit=bytecode` works on every program (main or library).
            rs_dis = subprocess.run(
                [str(mo_fe), "check", "--emit=bytecode", rel],
                cwd=ROOT,
                capture_output=True,
                timeout=60,
            )
            assert rs_dis.returncode == 0 and rs_dis.stderr == b"", (
                f"mo-fe check --emit=bytecode failed on {rel}: {rs_dis.stderr.decode(errors='replace')}"
            )
            assert rs_dis.stdout.startswith(b"bytecode 0.3 indexed\n"), (
                f"unexpected mo-fe check --emit=bytecode output on {rel}"
            )
            # Rust `mo-fe emit` produces `.bc` that passes `mo-rt verify`.
            r_emit = subprocess.run(
                [str(mo_fe), "emit", *emit_flags, rel, "-o", str(rs_bc)],
                cwd=ROOT,
                capture_output=True,
                timeout=60,
            )
            assert r_emit.returncode == 0 and r_emit.stderr == b"", (
                f"mo-fe emit failed on {rel}: {r_emit.stderr.decode(errors='replace')}"
            )
            r_ver = subprocess.run(
                [str(mo_rt), "verify", str(rs_bc)],
                cwd=ROOT,
                capture_output=True,
                timeout=30,
            )
            assert r_ver.returncode == 0 and r_ver.stderr == b"", (
                f"mo-rt verify failed on Rust-emitted {rel}: {r_ver.stderr.decode(errors='replace')}"
            )
            # Cross-verify Rust-emitted `.bc` with C `mo run` on `examples/greet.mo`!
            if rel == "examples/greet.mo":
                c_run_rs = subprocess.run(
                    [str(mo_bin), "run", str(rs_bc), "World"],
                    cwd=ROOT,
                    capture_output=True,
                    timeout=30,
                )
                c_run_src = subprocess.run(
                    [str(mo_bin), "run", rel, "World"],
                    cwd=ROOT,
                    capture_output=True,
                    timeout=30,
                )
                assert (
                    c_run_rs.returncode == c_run_src.returncode == 0
                    and c_run_rs.stdout == c_run_src.stdout
                    and c_run_rs.stderr == c_run_src.stderr == b""
                ), f"C mo run on Rust-emitted greet.bc differed: {c_run_rs} vs {c_run_src}"

        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            list(pool.map(verify_bc_one, enumerate(rt_programs)))

    return len(fixtures), len(generated), len(excluded), len(bi_paths), len(rt_programs)


def _e2e_golden_case(
    mo_bin: pathlib.Path,
    mo_fe: pathlib.Path,
    mo_rt: pathlib.Path,
    case: pathlib.Path,
    tmpdir: pathlib.Path,
) -> tuple[str, str, int, int, list[str]]:
    """Compile purely with `mo-fe emit` and execute purely with `mo-rt`; C `mo` is oracle only."""
    entry = (case / "entry").read_text().strip() if (case / "entry").exists() else None
    program = entry or str((case / "main.mo").relative_to(ROOT))
    command = check_rust_runtime.read_bytes(case, "command", b"run").decode().strip()
    assert command in ("run", "test"), f"{case.name}: unexpected command {command!r}"
    arguments = check_rust_runtime.read_bytes(case, "args").decode().splitlines()
    stdin_bytes = check_rust_runtime.read_bytes(case, "stdin")
    exp_stdout = check_rust_runtime.read_bytes(case, "stdout")
    exp_stderr = check_rust_runtime.read_bytes(case, "stderr")
    exp_status = int(check_rust_runtime.read_bytes(case, "status", b"0").decode().strip())

    category = check_rust_runtime.classify_golden_case(case)
    bc_path = tmpdir / f"e2e_golden_{case.name}.bc"
    emit_cmd = [str(mo_fe), "emit"]
    if command == "test":
        emit_cmd.append("--tests")
    emit_cmd.extend([program, "-o", str(bc_path)])

    if category == "compile-only":
        emitted = subprocess.run(emit_cmd, cwd=ROOT, capture_output=True, timeout=60)
        problems: list[str] = []
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

    emitted = subprocess.run(emit_cmd, cwd=ROOT, capture_output=True, timeout=60)
    if emitted.returncode != 0:
        return (
            category,
            command,
            0,
            0,
            [
                f"mo-fe emit failed (exit {emitted.returncode}): "
                f"{emitted.stderr.decode(errors='replace')}"
            ],
        )

    verified = subprocess.run(
        [str(mo_rt), "verify", str(bc_path)], cwd=ROOT, capture_output=True, timeout=30
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
            [f"mo-rt anchors failed (exit {anchors_res.returncode}): {anchors_res.stderr}"],
        )
    anchors_checked = check_rust_runtime.parse_anchors_count(anchors_res.stdout)

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
        rs_names = [
            m.group(2)
            for m in check_rust_runtime.TEST_LINE_RE.finditer(rs_res.stdout.decode(errors="replace"))
        ]
        exp_names = [
            m.group(2)
            for m in check_rust_runtime.TEST_LINE_RE.finditer(exp_stdout.decode(errors="replace"))
            if not check_rust_runtime.PROMISE_OR_SERVICE_NAME_RE.search(m.group(2))
        ]
        if rs_names != exp_names:
            problems.append(
                f"executed test/example inventory mismatch: expected {exp_names}, got {rs_names}"
            )
        else:
            problems.extend(
                check_rust_runtime.diff_test_only_batches(
                    mo_bin, mo_rt, program, bc_path, rs_names, ROOT, rs_full=rs_res
                )
            )
        return category, command, len(rs_names), anchors_checked, problems

    c_res = subprocess.run(
        [str(mo_bin), command, program, *arguments],
        cwd=ROOT,
        input=stdin_bytes,
        capture_output=True,
        timeout=120,
    )
    rt_cmd = [str(mo_rt), command, str(bc_path)]
    if command == "run" and arguments:
        rt_cmd.extend(["--", *arguments])
    rs_res = subprocess.run(
        rt_cmd, cwd=ROOT, input=stdin_bytes, capture_output=True, timeout=120
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
            f"--- pure-Rust\n{rs_res.stdout.decode(errors='replace')}"
        )
    if rs_res.stderr != c_res.stderr:
        problems.append(
            "stderr mismatch vs C mo\n"
            f"--- C mo\n{c_res.stderr.decode(errors='replace')}"
            f"--- pure-Rust\n{rs_res.stderr.decode(errors='replace')}"
        )
    if rs_res.returncode != c_res.returncode:
        problems.append(
            f"exit status mismatch vs C mo: C={c_res.returncode}, pure-Rust={rs_res.returncode}"
        )
    tests_checked = (
        len(check_rust_runtime.TEST_LINE_RE.findall(rs_res.stdout.decode(errors="replace")))
        if command == "test"
        else 0
    )
    return category, command, tests_checked, anchors_checked, problems


def _e2e_test_program(
    mo_bin: pathlib.Path,
    mo_fe: pathlib.Path,
    mo_rt: pathlib.Path,
    label: str,
    program: str,
    cwd: pathlib.Path,
    tmpdir: pathlib.Path,
    allow_empty: bool = False,
    require_all_pass: bool = True,
) -> tuple[int, int, int, list[str]]:
    """Pure-Rust `mo-fe emit --tests` -> `mo-rt verify/anchors/test`, diffed vs C `mo test`."""
    safe_name = re.sub(r"[^A-Za-z0-9_.-]+", "_", label)
    bc_path = tmpdir / f"e2e_test_{safe_name}.bc"

    emitted = subprocess.run(
        [str(mo_fe), "emit", "--tests", program, "-o", str(bc_path)],
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
                f"mo-fe emit --tests failed (exit {emitted.returncode}): "
                f"{emitted.stderr.decode(errors='replace')}"
            ],
        )

    verified = subprocess.run(
        [str(mo_rt), "verify", str(bc_path)], cwd=cwd, capture_output=True, timeout=30
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
    anchors_checked = check_rust_runtime.parse_anchors_count(anchors_res.stdout)

    rt_cmd = [str(mo_rt), "test"]
    if allow_empty:
        rt_cmd.append("--allow-empty")
    rt_cmd.append(str(bc_path))
    rs_res = subprocess.run(rt_cmd, cwd=cwd, capture_output=True, timeout=180)
    rs_out = rs_res.stdout.decode(errors="replace")
    rs_names = [m.group(2) for m in check_rust_runtime.TEST_LINE_RE.finditer(rs_out)]

    summary_match = check_rust_runtime.SUMMARY_RE.search(rs_out)
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

    listed = subprocess.run(
        [str(mo_bin), "test", "--list", program],
        cwd=cwd,
        capture_output=True,
        text=True,
        timeout=60,
    )
    if listed.returncode != 0:
        problems.append(f"mo test --list failed (exit {listed.returncode}): {listed.stderr}")
        return passed, len(rs_names), anchors_checked, problems

    list_names = [line.strip() for line in listed.stdout.splitlines() if line.strip()]
    has_non_bc_specs = any(
        check_rust_runtime.PROMISE_OR_SERVICE_NAME_RE.search(n) for n in list_names
    )
    expected_bc_names = [
        n for n in list_names if not check_rust_runtime.PROMISE_OR_SERVICE_NAME_RE.search(n)
    ]

    if rs_names != expected_bc_names:
        problems.append(
            f"test/example inventory mismatch vs `mo test --list`: "
            f"C={expected_bc_names}, pure-Rust={rs_names}"
        )
        return passed, len(rs_names), anchors_checked, problems

    if not has_non_bc_specs:
        c_res = subprocess.run([str(mo_bin), "test", program], cwd=cwd, capture_output=True, timeout=180)
        if rs_res.stdout != c_res.stdout:
            problems.append(
                "stdout mismatch vs C mo test\n"
                f"--- C mo\n{c_res.stdout.decode(errors='replace')}"
                f"--- pure-Rust\n{rs_out}"
            )
        if rs_res.stderr != c_res.stderr:
            problems.append(
                "stderr mismatch vs C mo test\n"
                f"--- C mo\n{c_res.stderr.decode(errors='replace')}"
                f"--- pure-Rust\n{rs_res.stderr.decode(errors='replace')}"
            )
        if rs_res.returncode != c_res.returncode:
            problems.append(
                f"exit status mismatch vs C mo test: C={c_res.returncode}, pure-Rust={rs_res.returncode}"
            )
    else:
        problems.extend(
            check_rust_runtime.diff_test_only_batches(
                mo_bin, mo_rt, program, bc_path, rs_names, cwd, rs_full=rs_res
            )
        )

    return passed, len(rs_names), anchors_checked, problems


def _e2e_lower_source_case(
    mo_bin: pathlib.Path,
    mo_fe: pathlib.Path,
    mo_rt: pathlib.Path,
    suite_dir: pathlib.Path,
    tmpdir: pathlib.Path,
) -> tuple[bool, int, int, list[str]]:
    name = suite_dir.name
    main_mo = suite_dir / "main.mo"
    program = str(main_mo.relative_to(ROOT))
    source_text = main_mo.read_text(encoding="utf-8")
    has_main = bool(re.search(r"^fn main\b", source_text, re.M))

    problems: list[str] = []
    ran_main = False
    if has_main:
        ran_main = True
        run_bc = tmpdir / f"e2e_lower_run_{name}.bc"
        emitted = subprocess.run(
            [str(mo_fe), "emit", program, "-o", str(run_bc)],
            cwd=ROOT,
            capture_output=True,
            timeout=60,
        )
        if emitted.returncode != 0:
            problems.append(
                f"mo-fe emit failed (exit {emitted.returncode}): "
                f"{emitted.stderr.decode(errors='replace')}"
            )
        else:
            verified = subprocess.run(
                [str(mo_rt), "verify", str(run_bc)], cwd=ROOT, capture_output=True, timeout=30
            )
            if verified.returncode != 0 or verified.stderr:
                problems.append(
                    f"mo-rt verify (run) failed (exit {verified.returncode}): "
                    f"{verified.stderr.decode(errors='replace')}"
                )
            else:
                c_res = subprocess.run(
                    [str(mo_bin), "run", program], cwd=ROOT, capture_output=True, timeout=60
                )
                rs_res = subprocess.run(
                    [str(mo_rt), "run", str(run_bc)], cwd=ROOT, capture_output=True, timeout=60
                )
                if name in check_rust_runtime.SERVICE_MAIN_EXPECTED_EXIT:
                    expected_rc = check_rust_runtime.SERVICE_MAIN_EXPECTED_EXIT[name]
                    if b"E1408:" not in c_res.stderr:
                        problems.append(
                            f"expected C mo run E1408 for service+main suite, got {c_res.stderr!r}"
                        )
                    if rs_res.returncode != expected_rc or rs_res.stdout != b"" or rs_res.stderr != b"":
                        problems.append(
                            f"mo-rt run mismatch for service+main suite: expected exit {expected_rc}, "
                            f"got exit={rs_res.returncode} stdout={rs_res.stdout!r} stderr={rs_res.stderr!r}"
                        )
                elif (rs_res.returncode, rs_res.stdout, rs_res.stderr) != (
                    c_res.returncode,
                    c_res.stdout,
                    c_res.stderr,
                ):
                    problems.append(
                        f"run mismatch vs C mo (C={c_res.returncode}, pure-Rust={rs_res.returncode})\n"
                        f"--- C stdout\n{c_res.stdout.decode(errors='replace')}"
                        f"--- pure-Rust stdout\n{rs_res.stdout.decode(errors='replace')}"
                        f"--- C stderr\n{c_res.stderr.decode(errors='replace')}"
                        f"--- pure-Rust stderr\n{rs_res.stderr.decode(errors='replace')}"
                    )

    _, tests_checked, anchors_checked, test_problems = _e2e_test_program(
        mo_bin,
        mo_fe,
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


def check_end_to_end_pure_rust_execution(
    mo_bin: pathlib.Path,
    mo_fe: pathlib.Path,
    mo_rt: pathlib.Path,
    modb_dir: pathlib.Path | None,
    jobs: int,
) -> dict[str, int]:
    """Slice 3A: 100% pure-Rust compile (`mo-fe emit`) + execute (`mo-rt`) parity across all corpora."""
    golden_dirs = sorted(p for p in (ROOT / "tests/golden").iterdir() if p.is_dir())
    assert len(golden_dirs) >= check_rust_runtime.MINIMUM_GOLDEN_SUITES
    lower_dirs = sorted(p for p in (ROOT / "tests/lower-source").iterdir() if p.is_dir())
    assert len(lower_dirs) >= check_rust_runtime.MINIMUM_LOWER_SOURCE_SUITES
    example_paths = sorted(
        [*(ROOT / "examples").glob("*.mo"), *(ROOT / "examples").glob("*/main.mo")]
    )
    assert len(example_paths) >= check_rust_runtime.MINIMUM_EXAMPLE_PROGRAMS
    lib_paths = sorted((ROOT / "lib").rglob("*.mo"))
    assert len(lib_paths) >= check_rust_runtime.MINIMUM_LIB_MODULES
    tool_service_paths = sorted(
        [
            *ROOT.glob("tools/*.mo"),
            *ROOT.glob("tools/*/main.mo"),
            *ROOT.glob("services/mo-*/main.mo"),
            ROOT / "services/task-queue/main.mo",
            ROOT / "services/task-queue/queue-tests.mo",
        ]
    )
    assert len(tool_service_paths) >= 17

    failures: list[str] = []
    stats = {
        "golden_full_run": 0,
        "golden_full_test": 0,
        "golden_promise_suites": 0,
        "golden_promise_tests": 0,
        "golden_compile_only": 0,
        "lower_suites": 0,
        "lower_mains": 0,
        "lower_tests": 0,
        "example_suites": 0,
        "example_tests": 0,
        "lib_suites": 0,
        "lib_tests": 0,
        "prog_suites": 0,
        "prog_tests": 0,
        "modb_suites": 0,
        "modb_tests": 0,
        "anchors": 0,
    }

    with tempfile.TemporaryDirectory(prefix="mo-rust-3a-e2e-") as td:
        tmpdir = pathlib.Path(td)
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            golden_futs = [
                (case, pool.submit(_e2e_golden_case, mo_bin, mo_fe, mo_rt, case, tmpdir))
                for case in golden_dirs
            ]
            lower_futs = [
                (d, pool.submit(_e2e_lower_source_case, mo_bin, mo_fe, mo_rt, d, tmpdir))
                for d in lower_dirs
            ]
            example_futs = [
                (
                    p,
                    pool.submit(
                        _e2e_test_program,
                        mo_bin,
                        mo_fe,
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
                        _e2e_test_program,
                        mo_bin,
                        mo_fe,
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
                        _e2e_test_program,
                        mo_bin,
                        mo_fe,
                        mo_rt,
                        f"prog_{p.relative_to(ROOT)}",
                        str(p.relative_to(ROOT)),
                        ROOT,
                        tmpdir,
                        allow_empty=(p == ROOT / "services/task-queue/main.mo"),
                    ),
                )
                for p in tool_service_paths
            ]
            modb_futs = []
            if modb_dir is not None:
                modb_root = modb_dir.resolve()
                assert modb_root.is_dir(), f"--modb-dir not found: {modb_root}"
                for rel in check_rust_runtime.MODB_SUITES:
                    assert (modb_root / rel).is_file(), f"missing MoDB suite: {modb_root / rel}"
                    modb_futs.append(
                        (
                            rel,
                            pool.submit(
                                _e2e_test_program,
                                mo_bin,
                                mo_fe,
                                mo_rt,
                                f"modb_{rel}",
                                rel,
                                modb_root,
                                tmpdir,
                            ),
                        )
                    )

            for case, fut in golden_futs:
                category, command, n_tests, n_anchors, probs = fut.result()
                stats["anchors"] += n_anchors
                if category == "compile-only":
                    stats["golden_compile_only"] += 1
                elif category == "promise-or-service":
                    stats["golden_promise_suites"] += 1
                    stats["golden_promise_tests"] += n_tests
                elif command == "run":
                    stats["golden_full_run"] += 1
                else:
                    stats["golden_full_test"] += 1
                if probs:
                    failures.append(f"e2e golden/{case.name} ({category}, {command}):\n  " + "\n  ".join(probs))

            for d, fut in lower_futs:
                ran_main, n_tests, n_anchors, probs = fut.result()
                stats["lower_suites"] += 1
                stats["lower_mains"] += int(ran_main)
                stats["lower_tests"] += n_tests
                stats["anchors"] += n_anchors
                if probs:
                    failures.append(f"e2e lower-source/{d.name}:\n  " + "\n  ".join(probs))

            for p, fut in example_futs:
                _, n_tests, n_anchors, probs = fut.result()
                stats["example_suites"] += 1
                stats["example_tests"] += n_tests
                stats["anchors"] += n_anchors
                if probs:
                    failures.append(f"e2e {p.relative_to(ROOT)}:\n  " + "\n  ".join(probs))

            for p, fut in lib_futs:
                _, n_tests, n_anchors, probs = fut.result()
                stats["lib_suites"] += 1
                stats["lib_tests"] += n_tests
                stats["anchors"] += n_anchors
                if probs:
                    failures.append(f"e2e {p.relative_to(ROOT)}:\n  " + "\n  ".join(probs))

            for p, fut in prog_futs:
                _, n_tests, n_anchors, probs = fut.result()
                stats["prog_suites"] += 1
                stats["prog_tests"] += n_tests
                stats["anchors"] += n_anchors
                if probs:
                    failures.append(f"e2e {p.relative_to(ROOT)}:\n  " + "\n  ".join(probs))

            for rel, fut in modb_futs:
                passed, _, n_anchors, probs = fut.result()
                stats["modb_suites"] += 1
                stats["modb_tests"] += passed
                stats["anchors"] += n_anchors
                if probs:
                    failures.append(f"e2e modb/{rel}:\n  " + "\n  ".join(probs))

    if failures:
        raise AssertionError("Pure-Rust E2E execution failures:\n\n" + "\n\n".join(failures))
    if modb_dir is not None:
        assert stats["modb_suites"] == 5 and stats["modb_tests"] == 29, (
            f"expected 5 MoDB suites / 29 passing tests, got {stats['modb_suites']} / {stats['modb_tests']}"
        )
    return stats


def check_mo_fe_check_latency(
    mo_fe: pathlib.Path, limit: float = 0.1, runs: int = 5
) -> list[tuple[str, float]]:
    """Run sequentially alone (after parallel work): `mo-fe check` stays under `limit` on all 12 samples."""
    samples = sorted((ROOT / "examples").glob("*.mo"))
    assert len(samples) >= 12, f"expected >= 12 samples in examples/, found {len(samples)}"
    results: list[tuple[str, float]] = []
    slow: list[str] = []
    print(f"mo-fe check latency, best of {runs} runs, limit {limit:.3f} s ({mo_fe.relative_to(ROOT)})")
    for sample in samples:
        rel = str(sample.relative_to(ROOT))
        best: float | None = None
        for _ in range(runs):
            t0 = time.perf_counter()
            res = subprocess.run([str(mo_fe), "check", rel], cwd=ROOT, capture_output=True, timeout=60)
            elapsed = time.perf_counter() - t0
            assert res.returncode == 0 and res.stderr == b"", (
                f"mo-fe check {sample.name} failed (rc={res.returncode}): "
                f"{res.stderr.decode(errors='replace')}"
            )
            best = elapsed if best is None else min(best, elapsed)
        assert best is not None
        over = best > limit
        if over:
            slow.append(f"{sample.name} ({best:.4f}s)")
        print(f"  {sample.name:<16} {best:7.4f} s{'  OVER' if over else ''}")
        results.append((sample.name, best))
    assert not slow, f"mo-fe check exceeded {limit:.3f} s limit: {', '.join(slow)}"
    print(f"all {len(samples)} samples within {limit:.3f} s")
    return results


def main() -> None:
    started = time.monotonic()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mo", help="Path to built C mo executable")
    parser.add_argument("--mo-fe", help="Path to built Rust mo-fe executable")
    parser.add_argument("--mo-rt", dest="mo_rt", help="Path to built Rust mo-rt executable")
    parser.add_argument("--mocore", help="Path to built Lean mocore executable")
    parser.add_argument("--mo-grammar", help="Path to built Bison mo-grammar executable")
    parser.add_argument("--modb-dir", help="Optional path to MoDB checkout for extra parity checks")
    parser.add_argument(
        "--skip-build", action="store_true", help="Do not build mo-fe, mo-rt, mocore, or mo-grammar"
    )
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
    mo_fe, mo_rt = find_or_build_mo_fe_and_rt(args.mo_fe, args.mo_rt, args.skip_build)
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
    err_total, err_front, err_post, err_non_check = check_all_error_diagnostics_parity(
        mo_bin, mo_fe, args.jobs
    )
    lib_n, ex_n, low_n, ts_n, gold_n = check_whole_corpus_full_parity(
        mo_bin, mo_fe, args.jobs
    )
    lower_corpus_n, lower_lean_n, lower_eval_n, lower_c_eval_n = (
        check_rust_lowering_and_lean_parity(mo_bin, mo_fe, c_check, mocore, args.jobs)
    )
    cg_fix_n, cg_gen_n, cg_exc_n, cg_bi_n, cg_rt_n = check_rust_codegen_and_bytecode_parity(
        mo_bin, mo_fe, mo_rt, args.jobs
    )
    modb_path = pathlib.Path(args.modb_dir) if args.modb_dir else None
    e2e = check_end_to_end_pure_rust_execution(mo_bin, mo_fe, mo_rt, modb_path, args.jobs)
    modb_summary = ""
    if modb_path is not None:
        modb_n, modb_lean_n = check_modb_full_parity(
            mo_bin, mo_fe, mocore, modb_path, args.jobs
        )
        modb_summary = (
            f" {modb_n} MoDB .mo files matched check --json + lowered ({modb_lean_n} Lean-checked)"
            f" + {e2e['modb_suites']} suites / {e2e['modb_tests']} tests passed 100% pure-Rust;"
        )

    # Timed sequentially alone, AFTER all parallel differential checks complete.
    latencies = check_mo_fe_check_latency(mo_fe)
    max_latency = max(sec for _, sec in latencies)

    oracle_desc = "C + Lean mocore" if mocore is not None else "C (Lean mocore unavailable)"
    grammar_desc = "Bison mo-grammar" if mo_grammar is not None else "expected verdicts"
    golden_total = (
        e2e["golden_full_run"]
        + e2e["golden_full_test"]
        + e2e["golden_promise_suites"]
        + e2e["golden_compile_only"]
    )
    print(
        f"Rust compiler 2A-2E+3A parity ({oracle_desc}, {grammar_desc}): "
        f"{crates_n} crates/{entries_n} entry points lint-audited; "
        f"{runs_n}/15 runs + {fuzz_n}/20 fuzz seeds round-tripped; "
        f"{diff_n} Core checker differential cases ({orig_n} corpus files, {prog_n} programs) matched; "
        f"{gen_n} generated Core programs matched & printed identically; "
        f"{emitted_n} C-emitted .core programs round-tripped ({accepted_n} standalone-checked); "
        f"{gram_n}/106 grammar fixtures ({gram_good} good, {gram_bad} bad) matched; "
        f"{tracked_n} tracked .mo files matched C mo format byte-for-byte ({fmt_ok} formatted+idempotent, {fmt_refused} refused); "
        f"{err_text_n} syntax/format error diagnostics ({err_json_n} JSON) matched byte-for-byte; "
        f"{err_total}/{err_total} tests/errors/ classified ({err_front} full check+codegen+anchor text+JSON byte-for-byte, "
        f"{err_post} post-check gaps, {err_non_check} non-check commands); "
        f"{lib_n} lib + {ex_n} examples + {low_n} lower-source + {ts_n} tools/services + {gold_n} golden matched full check --json byte-for-byte; "
        f"{lower_corpus_n} Rust-lowered programs verified ({lower_lean_n} Lean-typechecked, "
        f"{lower_eval_n} Lean eval outcomes matched, {lower_c_eval_n} vs C --emit=core); "
        f"codegen {cg_fix_n}/15 fixtures + {cg_gen_n} generated + {cg_exc_n} E1201 + {cg_bi_n} BigInt oracle + {cg_rt_n} C-.bc round-trip/disasm; "
        f"pure-Rust E2E {golden_total} golden ({e2e['golden_full_run']} run, {e2e['golden_full_test']} test, "
        f"{e2e['golden_promise_suites']} promise/service [{e2e['golden_promise_tests']} tests], {e2e['golden_compile_only']} compile-only) "
        f"+ {e2e['lower_suites']} lower-source ({e2e['lower_mains']} main, {e2e['lower_tests']} tests) "
        f"+ {e2e['example_suites']} examples ({e2e['example_tests']} tests) "
        f"+ {e2e['lib_suites']} lib ({e2e['lib_tests']} tests) "
        f"+ {e2e['prog_suites']} tools/services ({e2e['prog_tests']} tests) "
        f"+ {e2e['anchors']} anchors;"
        f"{modb_summary} "
        f"mo-fe check max latency {max_latency:.4f}s (<0.100s on {len(latencies)} samples); "
        f"{time.monotonic() - started:.2f}s"
    )


if __name__ == "__main__":
    main()
