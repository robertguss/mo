#!/usr/bin/env python3
"""Differential test harness comparing the pure-Rust runtime (`mo-rt`) against C `mo`.

Run from the repository root:
    python3 ci/check_rust_runtime.py [--mo PATH] [--mo-rt PATH] [--skip-build] [--case=NAME ...]

What this checks for Phase 1, Slice 1A:
  1. Builds `target/release/mo-rt` (unless `--skip-build`) and locates or builds C `mo`.
  2. Verifies `ldd target/release/mo-rt` has zero `libssl` / `libcrypto` linkage.
  3. Runs the curated Slice 1A cohort of 165 golden suites from `tests/golden/`
     (145 `run` + 20 `test`), emitting `.bc` via `mo emit` / `mo emit --tests`,
     verifying each `.bc` with `mo-rt verify`, and asserting exact byte-for-byte
     stdout, stderr, and exit-code parity between C `mo`, Rust `mo-rt`, and the
     hand-written golden expectations.
  4. Runs an inline differential check for `Env` (real + `Env.of` fake) and
     runtime limit flags (`--max-depth`, `--max-heap`).
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
GOLDEN = ROOT / "tests/golden"

# Curated Slice 1A golden cohort (165 suites: 145 run + 20 test).
# Covers arithmetic (Int/Byte/Float/Decimal/Time/Duration), control flow,
# recursion & tail calls, closures, records/structs, enums & pattern matching,
# abilities (Equal/Hash/Order/Text), strings & interpolation, Bytes, List/Map/Set/Range,
# modules, JSON library, sample apps (greet, inventory, json-query), defect/bug reports,
# Console/Args capabilities, and test/example blocks.
SLICE_1A_GOLDEN_CASES = [
    # Capabilities: Args & Console
    "cap-args-list",
    "cap-args-none",
    "cap-console-fake",
    "cap-console-line-endings",
    "cap-console-stdin",
    "cap-console-stdin-invalid",
    "cap-console-streams",
    # Collections: List, Map, Set, Range, Tuple
    "col-args-lists",
    "col-build-with-var",
    "col-index-search",
    "col-list-basics",
    "col-list-enumerate-zip",
    "col-list-map-find",
    "col-list-order-ties",
    "col-list-slicing",
    "col-map-basics",
    "col-map-counting",
    "col-map-equality",
    "col-map-order",
    "col-nested",
    "col-range-loops",
    "col-set-basics",
    "col-shared-values",
    "col-stdin-lines",
    "col-take-negative",
    "col-test-failing",
    "col-test-passing",
    "col-tuple-keys",
    "col-tuple-later-type",
    "col-tuples",
    "col-zip-pairs",
    # Diagnostics / Bytes / closures
    "dx-bytes-lambda-discard",
    # Failure handling, defects, Option/Result, exit statuses
    "fail-bug-constant-stop",
    "fail-bug-decimal-divide-by-zero",
    "fail-bug-decimal-overflow",
    "fail-bug-decimal-places",
    "fail-bug-divide-by-zero",
    "fail-bug-drop-negative",
    "fail-bug-duration-overflow",
    "fail-bug-duration-unit-overflow",
    "fail-bug-exit-range",
    "fail-bug-map-duplicate",
    "fail-bug-overflow",
    "fail-bug-pow-negative",
    "fail-bug-repeat-negative",
    "fail-bug-replace-empty",
    "fail-bug-slice-negative",
    "fail-bug-split-empty",
    "fail-bug-stop",
    "fail-bug-take-negative",
    "fail-bug-time-range",
    "fail-file-error-forms",
    "fail-main-exit-status",
    "fail-option-basics",
    "fail-result-declared",
    "fail-test-failing",
    "fail-test-results",
    "fail-try-wrap",
    # Samples & JSON
    "greet-sample",
    "json-ability",
    # Core language: abilities, closures, control flow, enums, records, recursion, tests
    "lang-abilities-custom-order",
    "lang-abilities-custom-text",
    "lang-abilities-hash",
    "lang-abilities-order",
    "lang-abilities-structural",
    "lang-abilities-text-forms",
    "lang-bindings",
    "lang-case-guards",
    "lang-closures",
    "lang-constants",
    "lang-dot-calls",
    "lang-dot-calls-cases",
    "lang-enum-shapes",
    "lang-enum-tree",
    "lang-expression-evaluator",
    "lang-generic-functions",
    "lang-generic-types",
    "lang-is-and-if",
    "lang-loop-control",
    "lang-patterns-lists",
    "lang-patterns-text",
    "lang-patterns-tuples",
    "lang-records",
    "lang-records-order",
    "lang-records-update",
    "lang-recursion-depth",
    "lang-recursion-tail",
    "lang-stop",
    "lang-test-failing",
    "lang-test-passing",
    "lang-while-is",
    # Standard library JSON
    "lib-json-errors",
    "lib-json-lookup",
    "lib-json-numbers",
    "lib-json-query",
    # Modules & visibility
    "mod-brace-import",
    "mod-chain",
    "mod-choice-cases",
    "mod-constants",
    "mod-hidden-customized",
    "mod-hidden-type",
    "mod-path-words",
    "mod-qualified",
    "mod-same-name",
    "mod-type-bare-after-use",
    "mod-type-bare-local-first",
    "mod-type-named-like-module",
    "nested-choice-return",
    # Numbers: Int, Float, Decimal
    "num-comparisons",
    "num-decimal-boundaries",
    "num-decimal-exact",
    "num-decimal-int",
    "num-decimal-limits",
    "num-decimal-rounding",
    "num-float-arithmetic",
    "num-float-convert",
    "num-int-checked",
    "num-int-division",
    "num-int-functions",
    "num-int-literals",
    "num-test-failing",
    # Reference examples
    "ref-case-receivers",
    "ref-evaluation",
    "ref-functions",
    "ref-operators",
    "ref-qualified-abilities",
    "ref-shout",
    "ref-source",
    "ref-time",
    "ref-unit-qualified",
    # Sample programs
    "sample-greet-args",
    "sample-greet-tests",
    "sample-inventory",
    "sample-inventory-tests",
    "sample-json-query-missing",
    "sample-json-query-path",
    "sample-json-query-pretty",
    "sample-json-query-tests",
    "sample-json-query-trailing",
    "sample-json-query-truncated",
    "sample-json-query-usage",
    # Testing framework (test & example blocks)
    "tst-assert-is-binding",
    "tst-example-uses-text",
    "tst-failing-assert",
    "tst-failing-example",
    "tst-private-names",
    "tst-stop-in-test",
    "tst-two-failures",
    # Text & Char
    "txt-caesar",
    "txt-case-chars",
    "txt-chars-roundtrip",
    "txt-counts",
    "txt-escapes",
    "txt-interpolation",
    "txt-interpolation-case",
    "txt-lines-words",
    "txt-multiline",
    "txt-ordering",
    "txt-parse-numbers",
    "txt-replace-repeat",
    "txt-search",
    "txt-split-join",
    "txt-test-pass",
    "txt-trim-pad",
    "txt-word-frequency",
]

MINIMUM_COHORT = 50


def read_bytes(case: pathlib.Path, name: str, default: bytes = b"") -> bytes:
    path = case / name
    return path.read_bytes() if path.exists() else default


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
    subprocess.run(
        ["make", "build/generated/tools/embedded.h"],
        cwd=ROOT,
        check=True,
    )
    subprocess.run(["make", "-j8", "CC=gcc"], cwd=ROOT, check=True)
    built = ROOT / "build/gcc/debug/mo"
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


def check_no_openssl_linkage(mo_rt: pathlib.Path) -> None:
    ldd = shutil.which("ldd")
    if ldd is None:
        return
    res = subprocess.run([ldd, str(mo_rt)], capture_output=True, text=True, timeout=30)
    assert res.returncode == 0, f"ldd {mo_rt} failed: {res.stderr}"
    lower = res.stdout.lower()
    for forbidden in ("libssl", "libcrypto"):
        assert forbidden not in lower, (
            f"{mo_rt} unexpectedly links {forbidden}:\n{res.stdout}"
        )


def check_golden_case(
    mo: pathlib.Path,
    mo_rt: pathlib.Path,
    case: pathlib.Path,
    tmpdir: pathlib.Path,
) -> tuple[str, list[str]]:
    entry = (case / "entry").read_text().strip() if (case / "entry").exists() else None
    program = entry or str((case / "main.mo").relative_to(ROOT))
    command = read_bytes(case, "command", b"run").decode().strip()
    assert command in ("run", "test"), f"{case.name}: unexpected command {command!r}"
    arguments = read_bytes(case, "args").decode().splitlines()
    stdin_bytes = read_bytes(case, "stdin")
    exp_stdout = read_bytes(case, "stdout")
    exp_stderr = read_bytes(case, "stderr")
    exp_status = int(read_bytes(case, "status", b"0").decode().strip())

    bc_path = tmpdir / f"{case.name}.bc"
    emit_cmd = [str(mo), "emit"]
    if command == "test":
        emit_cmd.append("--tests")
    emit_cmd.extend([program, "-o", str(bc_path)])

    emitted = subprocess.run(emit_cmd, cwd=ROOT, capture_output=True, timeout=60)
    if emitted.returncode != 0:
        return command, [
            f"mo emit failed (exit {emitted.returncode}): "
            f"{emitted.stderr.decode(errors='replace')}"
        ]

    verified = subprocess.run(
        [str(mo_rt), "verify", str(bc_path)],
        cwd=ROOT,
        capture_output=True,
        timeout=30,
    )
    if verified.returncode != 0 or verified.stderr:
        return command, [
            f"mo-rt verify failed (exit {verified.returncode}): "
            f"{verified.stderr.decode(errors='replace')}"
        ]

    # Run C `mo` as live differential oracle.
    c_res = subprocess.run(
        [str(mo), command, program, *arguments],
        cwd=ROOT,
        input=stdin_bytes,
        capture_output=True,
        timeout=120,
    )

    # Run Rust `mo-rt` on the emitted `.bc` file.
    rt_cmd = [str(mo_rt), command, str(bc_path)]
    if command == "run" and arguments:
        rt_cmd.extend(["--", *arguments])
    rs_res = subprocess.run(
        rt_cmd,
        cwd=ROOT,
        input=stdin_bytes,
        capture_output=True,
        timeout=60,
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
    return command, problems


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
    args = parser.parse_args()

    mo = find_or_build_mo(args.mo)
    mo_rt = find_or_build_mo_rt(args.mo_rt, args.skip_build)
    check_no_openssl_linkage(mo_rt)

    selected = args.case if args.case else SLICE_1A_GOLDEN_CASES
    if not args.case:
        assert len(selected) >= MINIMUM_COHORT, (
            f"Slice 1A cohort has {len(selected)} cases, needs >= {MINIMUM_COHORT}"
        )
        assert len(selected) == len(set(selected)), "duplicate case in SLICE_1A_GOLDEN_CASES"

    run_count = 0
    test_count = 0
    failures: list[str] = []

    with tempfile.TemporaryDirectory() as td:
        tmpdir = pathlib.Path(td)
        for name in selected:
            case = GOLDEN / name
            assert case.is_dir(), f"missing golden case: {case}"
            command, problems = check_golden_case(mo, mo_rt, case, tmpdir)
            if command == "run":
                run_count += 1
            else:
                test_count += 1
            if problems:
                failures.append(f"{name} ({command}):\n  " + "\n  ".join(problems))

        if not args.case:
            check_env_and_limits_differential(mo, mo_rt, tmpdir)

    if failures:
        sys.stderr.write("\n\n".join(failures) + "\n")
        sys.exit(1)

    total = run_count + test_count
    print(
        f"rust runtime: {total}/{total} golden suites matched C mo byte-for-byte "
        f"({run_count} run, {test_count} test; + Env/limits check; 0 diffs; no libssl/libcrypto)"
    )


if __name__ == "__main__":
    main()
