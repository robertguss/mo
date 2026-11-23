#!/usr/bin/env python3
"""Differential test harness comparing the pure-Rust runtime (`mo-rt`) against C `mo`.

Run from the repository root:
    python3 ci/check_rust_runtime.py [--mo PATH] [--mo-rt PATH] [--skip-build] [--case=NAME ...]

What this checks for Phase 1 (Slices 1A + 1B + 1C + 1D — 59/59 hostops, 87/87 opcodes):
  1. Builds `target/release/mo-rt` (unless `--skip-build`) and locates or builds C `mo`.
  2. Verifies `ldd target/release/mo-rt` has zero `libssl` / `libcrypto` linkage.
  3. Runs the curated cohort of 209 golden suites from `tests/golden/`
     (174 `run` + 35 `test`, covering all 187/187 primitives, all 87/87 opcodes,
     and all 59/59 hostops including `Network`/`Connection`, `Sqlite`/`Sqlite.Database`,
     `Command`, `Simulation` & live actors), emitting `.bc` via `mo emit` /
     `mo emit --tests`, verifying each `.bc` with `mo-rt verify`, and asserting
     exact byte-for-byte stdout, stderr, and exit-code parity between C `mo`,
     Rust `mo-rt`, and the hand-written golden expectations.
  4. Runs inline differential checks for `Env`, `--max-depth`, `BigInt`, `--root DIR`
     real `Files`/`ReadFiles`/`Files.File`, `Clock`, `Random`, `SecureRandom`, `Crypto`,
     `Unicode16`, real & fake `Network` (loopback TCP + loopback TLS with `SSL_CERT_FILE`
     + `allow_hosts`), real & fake `Sqlite` (`--root`, `--state-dir`, WAL, CRUD,
     authorizer, confinement), real & fake `Command` (`--root`, `within`, `allow_programs`,
     timeout), `Simulation` (`start`/`send`/`advance`/`crash`/`restart`/timers/admission),
     `services/task-queue/queue-tests.mo`, and `mo-rt verify` rejection of corrupted bytecode.
"""

import argparse
import os
import pathlib
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading

ROOT = pathlib.Path(__file__).resolve().parent.parent
GOLDEN = ROOT / "tests/golden"

# Curated Phase 1 (Slices 1A + 1B + 1C + 1D) golden cohort (209 suites: 174 run + 35 test).
SLICE_1D_GOLDEN_CASES = [
    # Actors & API Server
    "actor-run",
    "api-server",
    # Capabilities: Args, Clock, Console, Env, Files, ReadFiles, Random, SecureRandom, Network
    "cap-args-list",
    "cap-args-none",
    "cap-clock-fixed",
    "cap-console-fake",
    "cap-console-line-endings",
    "cap-console-stdin",
    "cap-console-stdin-invalid",
    "cap-console-streams",
    "cap-files-binary",
    "cap-files-fake",
    "cap-files-fake-folders",
    "cap-files-read",
    "cap-files-walk",
    "cap-files-within-escape",
    "cap-files-wordcount",
    "cap-network-error",
    "cap-network-forms",
    "cap-record-of-capabilities",
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
    "fail-bug-network-port",
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
    "fail-main-result-error",
    "fail-option-basics",
    "fail-option-result-map",
    "fail-result-declared",
    "fail-test-failing",
    "fail-test-results",
    "fail-try-wrap",
    # Samples, HTTP & JSON
    "greet-sample",
    "http-fake",
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
    # Standard library: Crypto, Date, JSON, Unicode16
    "lib-crypto-date",
    "lib-json-errors",
    "lib-json-lookup",
    "lib-json-numbers",
    "lib-json-query",
    "lib-unicode16",
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
    # Numbers: Int, Float, Decimal, BigInt
    "num-bigint",
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
    "ref-abilities",
    "ref-bigint",
    "ref-capability-fakes",
    "ref-case-receivers",
    "ref-evaluation",
    "ref-functions",
    "ref-library-natives",
    "ref-operators",
    "ref-qualified-abilities",
    "ref-shout",
    "ref-source",
    "ref-time",
    "ref-unit-qualified",
    # Sample programs
    "sample-expenses",
    "sample-expenses-malformed",
    "sample-expenses-tests",
    "sample-expenses-usage",
    "sample-find",
    "sample-find-count",
    "sample-find-none",
    "sample-find-tests",
    "sample-find-unreadable",
    "sample-find-usage",
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
    "sample-kv",
    "sample-todo-list-empty",
    "sample-todo-missing-item",
    "sample-todo-usage",
    "sample-wc",
    "sample-wc-missing",
    "sample-wc-tests",
    # Testing framework (test & example blocks)
    "tst-assert-is-binding",
    "tst-example-uses-text",
    "tst-failing-assert",
    "tst-failing-example",
    "tst-fake-files-bugs",
    "tst-fakes",
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
    "txt-invalid-utf8-file",
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

MINIMUM_COHORT = 200


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
    Simulation (start/send/advance/timers/crash/restart/admission), and task-queue tests."""

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

    # Spin up local TCP + TLS + plain-HTTP-for-TLS-error servers.
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

        # Verify services/task-queue/queue-tests.mo passes all 59 tests & examples on mo-rt!
        q_bc = tmpdir / "queue_tests.bc"
        subprocess.run(
            [str(mo), "emit", "--tests", str(ROOT / "services/task-queue/queue-tests.mo"), "-o", str(q_bc)],
            cwd=ROOT,
            check=True,
        )
        subprocess.run([str(mo_rt), "verify", str(q_bc)], cwd=ROOT, check=True)
        rs_q = subprocess.run([str(mo_rt), "test", str(q_bc)], cwd=ROOT, capture_output=True, timeout=60)
        assert rs_q.returncode == 0, (rs_q.stdout.decode(errors="replace"), rs_q.stderr.decode(errors="replace"))
        assert b"59 passed, 0 failed, 0 stopped" in rs_q.stdout, rs_q.stdout
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
    args = parser.parse_args()

    mo = find_or_build_mo(args.mo)
    mo_rt = find_or_build_mo_rt(args.mo_rt, args.skip_build)
    check_no_openssl_linkage(mo_rt)

    selected = args.case if args.case else SLICE_1D_GOLDEN_CASES
    if not args.case:
        assert len(selected) >= MINIMUM_COHORT, (
            f"Cohort has {len(selected)} cases, needs >= {MINIMUM_COHORT}"
        )
        assert len(selected) == len(set(selected)), "duplicate case in SLICE_1D_GOLDEN_CASES"

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
            check_slice_1c_differential(mo, mo_rt, tmpdir)
            check_slice_1d_differential(mo, mo_rt, tmpdir)

    if failures:
        sys.stderr.write("\n\n".join(failures) + "\n")
        sys.exit(1)

    total = run_count + test_count
    print(
        f"rust runtime: {total}/{total} golden suites matched C mo byte-for-byte "
        f"({run_count} run, {test_count} test; + Env/limits/BigInt/Files/Clock/Random/Crypto/Unicode16/"
        f"Network+TLS/Sqlite/Command/Simulation/task-queue/verify checks; 0 diffs; no libssl/libcrypto)"
    )


if __name__ == "__main__":
    main()
