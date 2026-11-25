#!/usr/bin/env python3
"""Workspace safety/linkage audit and pure-Rust <-> Lean MoCore differential test harness.

Run from the repository root:
    python3 ci/check_rust_frontend.py --audit-only [--mo PATH]
    python3 ci/check_rust_frontend.py [--mo PATH] [--mo-fe PATH] [--mocore PATH]
                                      [--mo-grammar PATH] [--skip-build]
                                      [--require-lean] [--lean-only]
                                      [--generated-count N] [--seed-start S] [--jobs N]

What this checks:
  1. Workspace lint, safety, and linkage policy audit (`--audit-only`, run in `ci/check.sh rust`):
       - Root `clippy.toml` configures test exemptions without weakening library lints.
       - Root `Cargo.toml` enforces hardened `writing-good-rust` `[workspace.lints]`
         including `unsafe_code = "forbid"` and `unwrap_used = "deny"`.
       - Every crate under `crates/` (17 crates) opts into `[lints] workspace = true` and puts
         `#![forbid(unsafe_code)]` in the first 5 lines of every entry point (`lib.rs`,
         `main.rs`, `build.rs`, `src/bin/*.rs`).
       - `ldd target/release/mo` links zero `libssl`, zero `libcrypto`, and zero `libsqlite3`.
  2. Lossless `.core` v0.11 canonical round-trip on all 15 `tests/core/` fixtures and
     all 20 `fuzz/corpus/fuzz_core_text/` seeds.
  3. Core typechecker differential parity (`mo-fe core-check-batch` vs Lean `mocore`).
  4. Generated well-typed Core programs (`ci/gen_core.py`, starting at `--seed-start`)
     typecheck & canonical print parity against Lean `mocore`.
  5. `mo check --emit=core` front-end seam verification across all real `.mo` programs.
  6. Pure-Rust lowering (`mo-lower`) verified against `mo-core` and Lean `mocore`
     (typechecking + semantic evaluation of all `check_lower_source.py` suites).
  7. In full mode (without `--lean-only`): grammar corpus parity (106/106 `grammar/tests/`
     vs Bison `mo-grammar`) and `< 0.1 s` `mo-fe check` latency gate.
"""

import argparse
import concurrent.futures
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
import check_lower_source  # noqa: E402
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
    assert len(crate_dirs) >= 17, f"expected >= 17 workspace crates, found {len(crate_dirs)}"
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


def check_no_forbidden_linkage(binary: pathlib.Path) -> None:
    """Verify `binary` dynamically links zero libssl, libcrypto, and libsqlite3."""
    ldd = shutil.which("ldd")
    if ldd is None:
        return
    res = subprocess.run([ldd, str(binary)], capture_output=True, text=True, timeout=30)
    assert res.returncode == 0, f"ldd {binary} failed: {res.stderr}"
    lower = res.stdout.lower()
    for forbidden in ("libssl", "libcrypto", "libsqlite3"):
        assert forbidden not in lower, (
            f"{binary} unexpectedly links {forbidden}:\n{res.stdout}"
        )


def find_or_build_mo(explicit: str | None, skip_build: bool = False) -> pathlib.Path:
    if explicit:
        path = pathlib.Path(explicit).resolve()
        assert path.is_file(), f"mo binary not found: {path}"
        return path
    built = ROOT / "target/release/mo"
    if not skip_build and not built.is_file():
        subprocess.run(
            ["cargo", "build", "--release", "--bin", "mo"],
            cwd=ROOT,
            check=True,
        )
    assert built.is_file(), f"failed to build {built}"
    return built


def find_or_build_mo_fe(explicit_fe: str | None, skip_build: bool) -> pathlib.Path:
    if explicit_fe is None and not skip_build:
        subprocess.run(
            ["cargo", "build", "--release", "--bin", "mo-fe"],
            cwd=ROOT,
            check=True,
        )
    mo_fe = pathlib.Path(explicit_fe).resolve() if explicit_fe else (ROOT / "target/release/mo-fe")
    assert mo_fe.is_file(), f"mo-fe binary not found: {mo_fe}"
    return mo_fe


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


def find_or_build_mo_grammar(
    explicit: str | None, skip_build: bool, require_bison: bool
) -> pathlib.Path | None:
    """Locate or build the Bison executable grammar (`build/grammar/mo-grammar`)."""
    if explicit:
        path = pathlib.Path(explicit).resolve()
        assert path.is_file(), f"mo-grammar binary not found: {path}"
        return path
    target = ROOT / "build/grammar/mo-grammar"
    if target.is_file():
        return target
    if not skip_build and shutil.which("bison") is not None:
        subprocess.run(
            ["make", "--no-print-directory", "-C", "grammar", "mo-grammar"],
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


def decode_framed_batch(data: bytes) -> list[tuple[bool, bytes | int]]:
    """Decode `mo-fe lower-batch` framed output: `R <len>\\n<bytes>` or `E <code>\\n`."""
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
            assert len(payload) == length, "truncated framed batch payload"
            pos += length
            results.append((True, payload))
        elif kind == "E":
            results.append((False, int(value)))
        else:
            raise AssertionError(f"unexpected framed batch header: {header!r}")
    return results


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
        return decode_framed_batch(res.stdout)

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        out: list[tuple[bool, bytes | int]] = []
        for items in pool.map(one, batches):
            out.extend(items)
    return out


def check_core_checker_differential(
    mo_fe: pathlib.Path,
    mocore: pathlib.Path | None,
    jobs: int,
) -> tuple[int, int, int]:
    """Compare mo-fe core-check verdicts & error codes against generator expectations and Lean mocore."""
    cases, originals, programs = collect_core_check_cases()
    with tempfile.TemporaryDirectory(prefix="mo-rust-core-check-") as tmp:
        files: list[str] = []
        for i, (_, raw, _) in enumerate(cases):
            p = pathlib.Path(tmp) / f"{i}.core"
            p.write_bytes(raw)
            files.append(str(p))

        rs_lines = run_batch_lines([str(mo_fe), "core-check-batch", "--kind", "run"], files, jobs)
        assert len(rs_lines) == len(cases), f"mo-fe returned {len(rs_lines)} of {len(cases)} verdicts"

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

        for i, ((label, raw, want), rs_line) in enumerate(zip(cases, rs_lines)):
            rs_parts = rs_line.split()
            rs_status = int(rs_parts[0])

            if want is not None:
                assert rs_status == want, (
                    f"{label}: generator expected {want}, mo-fe gave {rs_line!r}\n"
                    f"{raw.decode(errors='replace')}"
                )
            if lean_verdicts is not None:
                assert rs_status == lean_verdicts[i], (
                    f"{label}: verdict mismatch vs Lean ({lean_verdicts[i]}): "
                    f"mo-fe={rs_line!r}\n{raw.decode(errors='replace')}"
                )
            if rs_status in (4, 5):
                assert len(rs_parts) >= 2, (
                    f"{label}: missing error code in output: mo-fe={rs_line!r}"
                )
                if rs_status == 5:
                    code_num = int(rs_parts[1][1:])
                    assert code_num not in (1052, 1053), (
                        f"{label}: resource refusal is not a typing verdict: {rs_line}"
                    )
    return len(cases), originals, programs


def check_generated_core_programs(
    mo_fe: pathlib.Path,
    mocore: pathlib.Path | None,
    count: int,
    jobs: int,
    seed_start: int = 0,
) -> int:
    """Verify generated well-typed Core programs pass Rust and Lean checkers and print byte-identically."""
    assert count >= 1, "--generated-count must be >= 1"
    seeds = list(range(seed_start, seed_start + count))
    texts = [gen_core.document(seed) for seed in seeds]
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
        for i, rv in enumerate(rs_verdicts):
            assert rv == "0", f"generated seed {seeds[i]} refused: mo-fe={rv!r}\n{texts[i]}"

        rs_printed = run_batch_printed([str(mo_fe), "core-print-batch", "--kind", "run"], files, jobs)
        assert len(rs_printed) == count
        for i, (want, got_rs) in enumerate(zip(texts, rs_printed)):
            assert got_rs == want, f"mo-fe print mismatch on seed {seeds[i]}\nwant: {want}\ngot:  {got_rs}"

        if mocore is not None:
            def lean_one(item: tuple[int, str]) -> None:
                i, path = item
                rc, out = check_core_generated.lean(str(mocore), [path])
                assert rc in (0, 1, 2, 3), (
                    f"mocore refused generated seed {seeds[i]} (exit {rc}): {out}\n{texts[i]}"
                )
                rc2, canon = check_core_generated.lean(str(mocore), ["--canonical", path])
                assert rc2 == 0 and canon == texts[i], (
                    f"mocore canonical print mismatch on seed {seeds[i]} (exit {rc2})"
                )

            with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
                list(pool.map(lean_one, enumerate(files)))
    return count


def discover_mo_programs() -> list[tuple[str, str]]:
    """Collect real `.mo` entry points across examples/, tests/lower-source/, and tests/golden/."""
    programs: list[tuple[str, str]] = []

    examples_dir = ROOT / "examples"
    for p in sorted(examples_dir.glob("*.mo")):
        programs.append((f"example/{p.stem}", str(p.relative_to(ROOT))))
    for d in sorted(p for p in examples_dir.iterdir() if p.is_dir()):
        main_mo = d / "main.mo"
        if main_mo.is_file():
            programs.append((f"example/{d.name}", str(main_mo.relative_to(ROOT))))

    for d in sorted(p for p in (ROOT / "tests/lower-source").iterdir() if p.is_dir()):
        main_mo = d / "main.mo"
        if main_mo.is_file():
            programs.append((f"lower-source/{d.name}", str(main_mo.relative_to(ROOT))))

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


def check_emitted_core_programs(
    mo_bin: pathlib.Path,
    mo_fe: pathlib.Path,
    jobs: int,
) -> tuple[int, int]:
    """Emit `.core` via `mo check --emit=core`, verify byte-exact round-trip and Core checker verdicts."""
    entries = discover_mo_programs()
    assert len(entries) >= 250, f"expected >= 250 real .mo programs, found {len(entries)}"

    with tempfile.TemporaryDirectory(prefix="mo-rust-emit-core-") as tmp:
        tmp_path = pathlib.Path(tmp)

        def emit_one(item: tuple[int, tuple[str, str]]) -> tuple[str, pathlib.Path, bytes] | None:
            idx, (label, rel_prog) = item
            res = subprocess.run(
                [str(mo_bin), "check", "--emit=core", rel_prog],
                cwd=ROOT,
                capture_output=True,
                timeout=60,
            )
            if res.returncode != 0:
                if label.startswith("golden/"):
                    return None
                raise AssertionError(
                    f"mo check --emit=core failed on {label} ({rel_prog}): "
                    f"{res.stderr.decode(errors='replace')}"
                )
            raw = res.stdout
            assert raw.startswith(f"(core {check_core_check.VERSION} (Program ".encode()), (
                f"unexpected --emit=core output on {label}: {raw[:120]!r}"
            )
            prog_file = tmp_path / f"prog_{idx}.core"
            prog_file.write_bytes(raw)
            return label, prog_file, raw

        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            emitted = [item for item in pool.map(emit_one, enumerate(entries)) if item is not None]

        assert len(emitted) >= 250, f"expected >= 250 emitted .core programs, got {len(emitted)}"
        prog_files = [str(p) for _, p, _ in emitted]

        printed = run_batch_printed(
            [str(mo_fe), "core-print-batch", "--kind", "program"], prog_files, jobs
        )
        assert len(printed) == len(emitted)
        for (label, _, raw), got in zip(emitted, printed):
            assert got.encode("utf-8") == raw, f"mo-fe core-print round-trip diff on {label}"

        rs_verdicts = run_batch_lines(
            [str(mo_fe), "core-check-batch", "--kind", "program"], prog_files, jobs
        )
        accepted = 0
        for (label, _, raw), rv in zip(emitted, rs_verdicts):
            rs_parts = rv.split()
            if rs_parts[0] == "0":
                accepted += 1
            else:
                assert b"(sim (start " in raw or b"(sim (send " in raw, (
                    f"{label}: unexpected Core refusal without service descriptors: {rv!r}"
                )
                assert rs_parts[1] == "E1037", (
                    f"{label}: unexpected code on service Core: mo-fe={rv!r}"
                )
        return len(emitted), accepted


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


def check_rust_lowering_and_lean_parity(
    mo_bin: pathlib.Path,
    mo_fe: pathlib.Path,
    mocore: pathlib.Path | None,
    jobs: int,
) -> tuple[int, int, int, int]:
    """Verify Rust mo-lower across all corpora + Lean mocore typechecking & evaluation."""
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

        printed = run_batch_printed(
            [str(mo_fe), "core-print-batch", "--kind", "program"], prog_files, jobs
        )
        assert len(printed) == len(corpus)
        for (label, _), raw, got in zip(corpus, raw_docs, printed):
            assert got.encode("utf-8") == raw, f"canonical round-trip diff on Rust-lowered {label}"

        rs_verdicts = run_batch_lines(
            [str(mo_fe), "core-check-batch", "--kind", "program"], prog_files, jobs
        )
        standalone_runs: list[tuple[str, str]] = []
        for (label, _), raw, rf, rv in zip(corpus, raw_docs, run_files, rs_verdicts):
            rs_parts = rv.split()
            if rs_parts[0] == "0":
                standalone_runs.append((label, rf))
            else:
                assert b"(sim (start " in raw or b"(sim (send " in raw, (
                    f"{label}: unexpected standalone Core refusal on Rust-lowered output: {rv!r}"
                )
                assert rs_parts[1] == "E1037", (
                    f"{label}: expected E1037 on service-dependent Core, got mo-fe={rv!r}"
                )

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

        world = tmp_path / "world.core"
        world.write_text(f"(core {check_core_check.VERSION} {check_core_check.WORLD})\n")
        eval_matched = 0
        self_eval_matched = 0

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
            em = sm = 0
            if mocore is not None:
                got = _lean_oracle_eval(mocore, core_path, world)
                assert got == expected, f"case {name}: Lean want={expected}, got={got}"
                em = 1
                if (ROOT / "tests/lower-source" / name / "main.mo").is_file():
                    m_res = subprocess.run(
                        [str(mo_bin), "check", "--emit=core", "--lib", str(ROOT / "lib"), str(src)],
                        capture_output=True,
                        text=True,
                        timeout=30,
                    )
                    assert m_res.returncode == 0, f"mo check --emit=core failed on {name}: {m_res.stderr}"
                    m_core = cdir / "mo.core"
                    m_core.write_text(m_res.stdout)
                    m_got = _lean_oracle_eval(mocore, m_core, world)
                    assert got == m_got, f"case {name}: mo-fe={got} != mo={m_got}"
                    sm = 1
            return name, len(res.stdout.encode("utf-8")), em, sm

        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            for name, size, em, sm in pool.map(eval_case, check_lower_source.CASES.items()):
                lengths[name] = size
                eval_matched += em
                self_eval_matched += sm

        for family in ("bool", "pattern"):
            sizes = [lengths[f"linear_{family}_{n}"] for n in (4, 8, 16)]
            assert all(b <= 3 * a for a, b in zip(sizes, sizes[1:])), (
                f"non-linear lowering blowup in {family}: {sizes}"
            )

        def eval_spec(item: tuple[str, tuple[str, dict[str | None, list[str]]]]) -> int:
            name, (body, selections) = item
            sdir = tmp_path / f"spec_{name}"
            sdir.mkdir()
            src = sdir / "main.mo"
            src.write_bytes(("module Main\n" + body + "\n").encode("utf-8"))
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

    return len(corpus), lean_typechecked, eval_matched, self_eval_matched


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
    parser.add_argument("--mo", help="Path to built mo executable")
    parser.add_argument("--mo-fe", help="Path to built mo-fe executable")
    parser.add_argument("--mocore", help="Path to built Lean mocore executable")
    parser.add_argument("--mo-grammar", help="Path to built Bison mo-grammar executable")
    parser.add_argument(
        "--skip-build", action="store_true", help="Do not build mo, mo-fe, mocore, or mo-grammar"
    )
    parser.add_argument(
        "--audit-only",
        action="store_true",
        help="Run only workspace lint/safety policy audit and binary linkage check",
    )
    parser.add_argument(
        "--lean-only",
        action="store_true",
        help="Run only Lean MoCore differential parity checks (fixtures, Core checker, generated Core, lowering)",
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
        "--seed-start",
        type=int,
        default=int(os.environ.get("MO_CORE_DIFF_SEED", "0")),
        help="First ci/gen_core.py seed to cross-check (default MO_CORE_DIFF_SEED or 0)",
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
    if args.audit_only:
        mo_bin = find_or_build_mo(args.mo, skip_build=True)
        check_no_forbidden_linkage(mo_bin)
        print(
            f"Rust safety & linkage audit: {crates_n} crates/{entries_n} entry points "
            f"(#![forbid(unsafe_code)], [lints] workspace = true); "
            f"{mo_bin.relative_to(ROOT)} clean (zero libssl/libcrypto/libsqlite3)"
        )
        return

    mo_bin = find_or_build_mo(args.mo, args.skip_build)
    mo_fe = find_or_build_mo_fe(args.mo_fe, args.skip_build)
    mocore = find_or_build_mocore(args.mocore, args.skip_build, require_tools)

    runs_n, fuzz_n = check_committed_fixtures_and_fuzz_seeds(mo_fe)
    diff_n, orig_n, prog_n = check_core_checker_differential(mo_fe, mocore, args.jobs)
    gen_n = check_generated_core_programs(
        mo_fe, mocore, args.generated_count, args.jobs, seed_start=args.seed_start
    )
    emitted_n, accepted_n = check_emitted_core_programs(mo_bin, mo_fe, args.jobs)
    lower_corpus_n, lower_lean_n, lower_eval_n, lower_self_eval_n = (
        check_rust_lowering_and_lean_parity(mo_bin, mo_fe, mocore, args.jobs)
    )
    seed_end = args.seed_start + gen_n - 1

    if args.lean_only:
        print(
            f"Rust <-> Lean MoCore differential parity: "
            f"{runs_n}/15 runs + {fuzz_n}/20 fuzz seeds round-tripped; "
            f"{diff_n} Core checker cases ({orig_n} corpus, {prog_n} programs) matched Lean; "
            f"{gen_n} generated Core programs (seeds {args.seed_start}..{seed_end}) matched & printed identically; "
            f"{emitted_n} emitted .core programs round-tripped ({accepted_n} standalone-checked); "
            f"{lower_corpus_n} Rust-lowered programs verified ({lower_lean_n} Lean-typechecked, "
            f"{lower_eval_n} Lean eval outcomes matched, {lower_self_eval_n} self-consistent); "
            f"{time.monotonic() - started:.2f}s"
        )
        return

    mo_grammar = find_or_build_mo_grammar(args.mo_grammar, args.skip_build, require_tools)
    gram_n, gram_good, gram_bad = check_grammar_corpus_parity(mo_fe, mo_grammar)
    latencies = check_mo_fe_check_latency(mo_fe)
    max_latency = max(sec for _, sec in latencies)

    print(
        f"Rust frontend & Lean parity: "
        f"{crates_n} crates/{entries_n} entry points lint-audited; "
        f"{runs_n}/15 runs + {fuzz_n}/20 fuzz seeds round-tripped; "
        f"{diff_n} Core checker cases ({orig_n} corpus, {prog_n} programs) matched; "
        f"{gen_n} generated Core programs (seeds {args.seed_start}..{seed_end}) matched & printed identically; "
        f"{emitted_n} emitted .core programs round-tripped ({accepted_n} standalone-checked); "
        f"{lower_corpus_n} Rust-lowered programs verified ({lower_lean_n} Lean-typechecked, "
        f"{lower_eval_n} Lean eval outcomes matched, {lower_self_eval_n} self-consistent); "
        f"{gram_n}/106 grammar fixtures ({gram_good} good, {gram_bad} bad) matched; "
        f"mo-fe check max latency {max_latency:.4f}s (<0.100s on {len(latencies)} samples); "
        f"{time.monotonic() - started:.2f}s"
    )


if __name__ == "__main__":
    main()
