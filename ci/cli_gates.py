import json
import subprocess

from cli_common import records, root, run

GATES = "tests/cli/gates"
SURVIVOR = "tests/errors/e1439/main.mo"
NOT_GATED = b"gates: did not run: the run selects checks or sets a limit\n"


def totals(binary, path, status):
    items = records(run(binary, ["test", "--gates", "--json", path], status), "test")
    return items, items[-1]


def mutants(total, tried, survived=0, dropped=0, **killed):
    assert total["gates"] == "ran", total
    kinds = {"promise": 0, "example": 0, "test": 0, "recovery": 0, "start": 0, **killed}
    assert total["mutants"] == {"tried": tried, "killed": kinds, "survived": survived,
                                "dropped": dropped}, total
    assert sum(kinds.values()) + survived == tried, total


def gates(binary):
    items, total = totals(binary, SURVIVOR, 1)
    survivors = [r for r in items if r["record"] == "diagnostic"]
    assert [(r["code"], r["primary"]["line"], r["primary"]["label"]) for r in survivors] == [
        ("E1439", 9, "Main.small: boundary, `<` to `<=`")], survivors
    mutants(total, 2, survived=1, test=1)
    again, _ = totals(binary, SURVIVOR, 1)
    for listed in (items, again):
        for r in listed:
            r.pop("microseconds", None)
    assert json.dumps(items) == json.dumps(again)
    for args in (["--only", "Main.test.1"], ["--in", "Main"], ["--calls", "10"],
                 ["--seconds", "5"], ["--max-depth", "100"]):
        result = run(binary, ["test", "--gates", *args, SURVIVOR], 0)
        assert not result.stderr, (args, result.stderr)
        assert result.stdout.splitlines(keepends=True)[-2] == NOT_GATED, (args, result.stdout)
    skipped = records(run(binary, ["test", "--gates", "--json", "--only", "Main.test.1", SURVIVOR],
                          0), "test")[-1]
    assert skipped["gates"] == "did not run" and "mutants" not in skipped, skipped
    plain = records(run(binary, ["test", "--json", SURVIVOR], 0), "test")[-1]
    assert "gates" not in plain, plain
    for flag in (["--strength", "x"], ["--bundle"], ["--list"]):
        result = run(binary, ["test", "--gates", *flag, SURVIVOR], 2)
        assert result.stderr == (b"--gates runs every check and tries every mutant; it takes no "
                                 b"--list, --bundle or --strength: \"--gates\"\n"), result.stderr

    _, total = totals(binary, f"{GATES}/killed/main.mo", 0)
    mutants(total, 3, promise=1, example=1, test=1)

    items, total = totals(binary, f"{GATES}/unrun/main.mo", 1)
    assert [r["primary"]["label"] for r in items if r["record"] == "diagnostic"] == [
        "Main.neither: not, `not` removed; no check runs this function",
        "Main.neither: logic, `or` to `and`; no check runs this function"], items
    mutants(total, 2, survived=2)

    failing = f"{GATES}/failing/main.mo"
    plain = run(binary, ["test", failing], 1)
    gated = run(binary, ["test", "--gates", failing], 1)
    lines = plain.stdout.splitlines(keepends=True)
    assert b"  input n = 100\n" in lines, plain.stdout
    assert gated.stdout == b"".join(
        lines[:-1] + [b"gates: did not run: a check did not pass\n", lines[-1]]), gated.stdout
    assert gated.stderr == plain.stderr == b""

    _, total = totals(binary, f"{GATES}/loops/main.mo", 0)
    mutants(total, 4, example=4)
    _, total = totals(binary, f"{GATES}/divide/main.mo", 0)
    mutants(total, 2, example=2)
    _, total = totals(binary, f"{GATES}/dropped/main.mo", 0)
    mutants(total, 0, dropped=1)
    _, total = totals(binary, f"{GATES}/anchors/main.mo", 0)
    mutants(total, 4, promise=1, start=3)
    _, total = totals(binary, f"{GATES}/empty/main.mo", 0)
    mutants(total, 0)

    library = subprocess.run([str(binary), "test", "--gates", "--json", "lib/list.mo"], cwd=root,
                             capture_output=True, timeout=300)
    items = records(library, "test")
    total = items[-1]
    killed = sum(total["mutants"]["killed"].values())
    assert library.returncode == 1 and killed > 0, total
    assert {r["code"] for r in items if r["record"] == "diagnostic"} == {"E1439"}, items
    assert total["mutants"]["dropped"] == 1 and total["errors"] == total["mutants"]["survived"]
