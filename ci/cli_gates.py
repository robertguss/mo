import json
import pathlib
import struct
import subprocess
import tempfile

from cli_common import records, root, run

GATES = "tests/cli/gates"
SURVIVOR = "tests/errors/e1439/main.mo"


def bundle(folder, entry, files):
    def field(data):
        return struct.pack("<I", len(data)) + data
    wire = b"MOSB\1" + field(entry.encode()) + struct.pack("<I", len(files))
    for name in sorted(files):
        wire += field(name.encode()) + field((folder / name).read_bytes())
    return wire


def totals(binary, path, status):
    items = records(run(binary, ["test", "--mutants", "--json", path], status), "test")
    return items, items[-1]


def check_mutants(total, tried, survived=0, dropped=0, **killed):
    kinds = {"promise": 0, "example": 0, "test": 0, "recovery": 0, "start": 0, **killed}
    assert total["mutants"] == {"tried": tried, "killed": kinds, "survived": survived,
                                "dropped": dropped}, total
    assert sum(kinds.values()) + survived == tried, total


def mutants(binary):
    # Plain `mo test` runs checks only and emits no mutant pass or E1439/E1440 diagnostics.
    plain = run(binary, ["test", SURVIVOR], 0)
    assert not plain.stderr and b"mutants:" not in plain.stdout and b"gates:" not in plain.stdout
    plain_json = records(run(binary, ["test", "--json", SURVIVOR], 0), "test")[-1]
    assert "mutants" not in plain_json and "gates" not in plain_json, plain_json

    items, total = totals(binary, SURVIVOR, 1)
    survivors = [r for r in items if r["record"] == "diagnostic"]
    assert [(r["code"], r["primary"]["line"], r["primary"]["label"]) for r in survivors] == [
        ("E1439", 9, "Main.small: boundary, `<` to `<=`")], survivors
    check_mutants(total, 2, survived=1, test=1)
    again, _ = totals(binary, SURVIVOR, 1)
    for listed in (items, again):
        for r in listed:
            r.pop("microseconds", None)
    assert json.dumps(items) == json.dumps(again)

    refused = (b"--mutants runs every check and tries every mutant; it takes no -o, "
               b"--only, --in, --calls, --seconds, --max-stack, --max-depth, --max-heap, "
               b"--bundle or --strength: \"--mutants\"\n")
    for args in (["--only", "Main.test.1"], ["--in", "Main"], ["--calls", "10"],
                 ["--seconds", "5"], ["--max-depth", "100"], ["--bundle"], ["--strength", "x"],
                 ["-o", "a.bc"]):
        assert run(binary, ["test", "--mutants", *args, SURVIVOR], 2).stderr == refused, args
    assert run(binary, ["test", "--gates", SURVIVOR], 2).stderr == b'unknown flag: "--gates"\n'
    assert run(binary, ["test", "--against", SURVIVOR, SURVIVOR], 2).stderr == (
        b'flag is not accepted by this command: "--against"\n')
    strength = run(binary, ["test", SURVIVOR, "--strength", "tests/cli/strength/versions"], 0)
    assert b"mutants" not in strength.stdout and not strength.stderr, strength
    with tempfile.TemporaryDirectory() as scratch:
        survivor = pathlib.Path(scratch) / "survivor.mosb"
        survivor.write_bytes(bundle(root / "tests/errors/e1439", "main.mo", ["main.mo"]))
        door = run(binary, ["test", "--bundle", survivor], 0)
    assert b"mutants" not in door.stdout and not door.stderr, door

    items, total = totals(binary, f"{GATES}/killed/main.mo", 0)
    check_mutants(total, 3, promise=1, example=1, test=1)
    assert [(r["function"], r["kind"], r["outcome"], r["by"], r["check"]) for r in items
            if r["record"] == "mutant"] == [
        ("Main.both", "logic", "killed", "promise", "Main.both.promise.1"),
        ("Main.add", "arithmetic", "killed", "example", "Main.add.example.1"),
        ("Main.flip", "not", "killed", "test", "Main.test.1")], items

    items, total = totals(binary, f"{GATES}/unrun/main.mo", 1)
    assert [r["primary"]["label"] for r in items if r["record"] == "diagnostic"] == [
        "Main.neither: not, `not` removed; no check runs this function",
        "Main.neither: logic, `or` to `and`; no check runs this function",
        "Main.head: int, `0` to `1`; no check runs this function"], items
    check_mutants(total, 3, survived=3, dropped=1)
    assert [(r["line"], r["column"], r["original"], r["replacement"], r["outcome"]) for r in items
            if r["record"] == "mutant"] == [
        (4, 3, "not", "", "survived"), (4, 10, "or", "and", "survived"),
        (8, 12, "or", "and", "dropped"), (8, 15, "0", "1", "survived")], items
    plain_list = run(binary, ["test", "--list", f"{GATES}/unrun/main.mo"], 0)
    assert plain_list.stdout == b"Main.test.1\n", plain_list.stdout
    listing = run(binary, ["test", "--mutants", "--list", f"{GATES}/unrun/main.mo"], 0)
    assert listing.stdout.decode().splitlines() == [
        "Main.test.1",
        f"mutant {GATES}/unrun/main.mo:4:3 Main.neither: not, `not` removed",
        f"mutant {GATES}/unrun/main.mo:4:10 Main.neither: logic, `or` to `and`",
        f"mutant {GATES}/unrun/main.mo:8:12 Main.head: logic, `or` to `and`",
        f"mutant {GATES}/unrun/main.mo:8:15 Main.head: int, `0` to `1`",
        "function Main.neither: 2 mutants", "function Main.head: 2 mutants",
        "function Main.main: 0 mutants"], listing.stdout
    listed = records(run(binary, ["test", "--mutants", "--list", "--json",
                                  f"{GATES}/unrun/main.mo"], 0), "test")
    assert [r for r in listed if r["record"] == "mutant"] == [
        {k: v for k, v in r.items() if k not in ("outcome", "by", "check")} for r in items
        if r["record"] == "mutant"], listed
    assert [(r["name"], r["mutants"]) for r in listed if r["record"] == "function"] == [
        ("Main.neither", 2), ("Main.head", 2), ("Main.main", 0)], listed

    failing = run(binary, ["test", "--mutants", f"{GATES}/failing/main.mo"], 1)
    lines = failing.stdout.splitlines(keepends=True)
    assert b"  input n = 100\n" in lines, failing.stdout
    assert lines[-2] == b"mutants: did not run: a check did not pass\n" and not failing.stderr, lines
    failing_json = records(run(binary, ["test", "--mutants", "--json",
                                        f"{GATES}/failing/main.mo"], 1), "test")[-1]
    assert failing_json["mutants"] is None, failing_json

    _, total = totals(binary, f"{GATES}/loops/main.mo", 0)
    check_mutants(total, 4, example=4)
    _, total = totals(binary, f"{GATES}/divide/main.mo", 0)
    check_mutants(total, 2, example=2)
    _, total = totals(binary, f"{GATES}/dropped/main.mo", 0)
    check_mutants(total, 0, dropped=1)
    _, total = totals(binary, f"{GATES}/anchors/main.mo", 0)
    check_mutants(total, 4, promise=1, start=3)
    _, total = totals(binary, f"{GATES}/empty/main.mo", 0)
    check_mutants(total, 0)

    # Exposed functions and service tools without examples/promises pass both `mo test` and
    # `mo test --mutants` whenever tests exercise their code (no E1440 gate).
    with tempfile.TemporaryDirectory(prefix="mo-mutants-exposed-") as scratch:
        path = pathlib.Path(scratch) / "main.mo"
        path.write_text(
            "module Main\nexpose owner, add\n\n"
            "struct Account\n  owner: Text\nend\n\n"
            "fn owner(a: Account) : Text\n  a.owner\nend\n\n"
            "fn add(a: Int, b: Int) : Int\n  a + b\nend\n\n"
            "test \"exercised by test\"\n"
            "  assert owner(Account(owner: \"alice\")) == \"alice\"\n"
            "  assert add(2, 3) == 5\n"
            "end\n")
        assert run(binary, ["test", str(path)], 0).stderr == b""
        _, total = totals(binary, str(path), 0)
        check_mutants(total, 1, test=1)

    library = subprocess.run([str(binary), "test", "--mutants", "--json", "lib/list.mo"], cwd=root,
                             capture_output=True, timeout=300)
    items = records(library, "test")
    total = items[-1]
    killed = sum(total["mutants"]["killed"].values())
    assert library.returncode == 0 and killed > 0 and total["mutants"]["survived"] == 0, total
    assert not [r for r in items if r["record"] == "diagnostic"], items
    assert total["mutants"]["dropped"] == 1 and total["errors"] == 0, total
