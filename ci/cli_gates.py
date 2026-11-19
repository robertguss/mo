import hashlib
import json
import pathlib
import shutil
import struct
import subprocess
import tempfile

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
        "Main.neither: logic, `or` to `and`; no check runs this function",
        "Main.head: int, `0` to `1`; no check runs this function"], items
    mutants(total, 3, survived=3, dropped=1)

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


def unstated(items):
    return [(r["headline"], r["primary"]["line"], r["primary"]["label"]) for r in items
            if r["record"] == "diagnostic"]


def stated_intent(binary):
    exposed = "an exposed function states no intent"
    items, total = totals(binary, f"{GATES}/intent/main.mo", 1)
    assert unstated(items) == [
        (exposed, 20, "Main.size@Crate: give it a promise or an example"),
        (exposed, 24, "Main.bare: give it a promise or an example"),
        (exposed, 28, "Main.also_bare: give it a promise or an example")], items
    assert {r["code"] for r in items if r["record"] == "diagnostic"} == {"E1440"}, items
    assert total["gates"] == "did not run" and "mutants" not in total, total
    text = run(binary, ["test", "--gates", f"{GATES}/intent/main.mo"], 1).stdout.splitlines()
    assert text[-2] == b"gates: did not run: a function or tool states no intent", text
    for program in ("intent", "tools"):
        path = f"{GATES}/{program}/main.mo"
        assert not run(binary, ["check", path], 0).stderr
        result = run(binary, ["test", "--gates", "--in", "Main", path], 0)
        assert not result.stderr and result.stdout.splitlines(keepends=True)[-2] == NOT_GATED
    ran = run(binary, ["run", f"{GATES}/intent/main.mo"], 0)
    assert not ran.stdout and not ran.stderr, ran

    items, total = totals(binary, f"{GATES}/tools/main.mo", 1)
    assert unstated(items) == [
        (exposed, 28, "Main.add: give it a promise or an example"),
        ("a service tool states no intent", 36,
         "Main.peek: write a service promise on peek, or give it a promise or an example")], items
    assert total["passed"] == 5 and total["gates"] == "did not run", total

    library = subprocess.run([str(binary), "test", "--gates", "--json", "lib/json.mo"], cwd=root,
                             capture_output=True, timeout=300)
    items = records(library, "test")
    assert library.returncode == 1 and [(h, label) for h, _, label in unstated(items)] == [
        (exposed, "Json.to_text@JsonNumber: give it a promise or an example")], items


BASE = root / "tests/cli/against/base/main.mo"
TRIPLE = ("fn triple(n: Int) : Int\n  examples\n    triple(2) == 6\n  end\n\n  n * 3\nend\n\n"
          "fn main()")


def edit(text, *pairs):
    for old, new in pairs:
        assert old in text, old
        text = text.replace(old, new)
    return text


def bundle_digest(folder, entry, files):
    def field(data):
        return struct.pack("<I", len(data)) + data
    wire = b"MOSB\1" + field(entry.encode()) + struct.pack("<I", len(files))
    for name in sorted(files):
        wire += field(name.encode()) + field((folder / name).read_bytes())
    return hashlib.sha256(wire).hexdigest()


def snapshot(folder):
    return {p: p.is_file() and p.read_bytes() for p in sorted(folder.rglob("*"))}


def against(binary, old, new, status, json_output=True):
    with tempfile.TemporaryDirectory() as scratch:
        folder = pathlib.Path(scratch)
        for name, text in (("old", old), ("new", new)):
            (folder / name).mkdir()
            (folder / name / "main.mo").write_text(text)
        before = snapshot(folder)
        args = ["test", f"{folder}/new/main.mo", "--against", f"{folder}/old/main.mo"]
        result = run(binary, args + (["--json"] if json_output else []), status)
        assert snapshot(folder) == before
        digest = bundle_digest(folder / "old", "main.mo", ["main.mo"])
        if not json_output:
            return result, digest
        items = records(result, "test")
        changes = [r for r in items if r["record"] == "changes"]
        assert len(changes) <= 1 and all(r["previous"] == digest for r in changes), changes
        return items, digest


def shorter(total, digest, tried, carried, survived=0, **killed):
    assert total["gates"] == "ran shorter" and total["previous"] == digest, total
    assert total["carried"] == carried, total
    kinds = {"promise": 0, "example": 0, "test": 0, "recovery": 0, "start": 0, **killed}
    assert total["mutants"] == {"tried": tried, "killed": kinds, "survived": survived,
                                "dropped": 0}, total


def shorter_run(binary):
    base = BASE.read_text()
    changed = edit(base, ("  a + b\n", "  b + a\n"))
    items, digest = against(binary, base, changed, 0)
    shorter(items[-1], digest, 1, 4, example=1)
    again, _ = against(binary, base, changed, 0)
    for listed in (items, again):
        for r in listed:
            r.pop("microseconds", None)
            r.get("span", {}).clear()
    assert items == again
    result, digest = against(binary, base, changed, 0, json_output=False)
    lines = result.stdout.decode().splitlines()
    assert lines[-3:] == [
        f"gates: not the full run: 4 functions carried over untried from previous version {digest}",
        "gates: 1 mutants tried, 1 killed (0 promise, 1 example, 0 test, 0 recovery, 0 start), "
        "0 survived, 0 dropped", "8 passed, 0 failed, 0 stopped"], lines

    removed = edit(base, ('test "twice zero is zero"\n  assert twice(0) == 0\nend\n', ""))
    items, digest = against(binary, base, removed, 0)
    shorter(items[-1], digest, 2, 4, test=2)
    layout = edit(base, ("  a < b\n", "  # compare\n\n  a   <   b\n"))
    items, digest = against(binary, base, layout, 0)
    shorter(items[-1], digest, 0, 5)
    items, digest = against(binary, base, base, 0)
    shorter(items[-1], digest, 0, 5)
    shifted = edit(base, ('  promise "never negative"',
                          '  promise "never below n"\n    result >= n\n    allows n: 3, result: 3\n'
                          '    forbids n: 3, result: 2\n  end\n\n  promise "never negative"'))
    items, digest = against(binary, base, shifted, 0)
    shorter(items[-1], digest, 1, 4, example=1)
    renamed = edit(base, ("fn twice(n", "fn double(n"), ("six?(twice(3))", "six?(double(3))"),
                   ("twice(0)", "double(0)"),
                   ("fn main()", TRIPLE))
    items, digest = against(binary, base, renamed, 0)
    shorter(items[-1], digest, 6, 3, example=2, test=4)
    helper = edit(base, ("fn six?(n: Int) : Bool\n  n == 6\n", "fn six?(_n: Int) : Bool\n  true\n"))
    items, digest = against(binary, base, helper, 1)
    assert [r["primary"]["label"] for r in items if r["record"] == "diagnostic"] == [
        "Main.twice: arithmetic, `*` to `/`", "Main.twice: int, `2` to `3`"], items
    shorter(items[-1], digest, 3, 3, survived=2, test=1)
    unused = edit(base, ("fn main()", "fn unused(a: Int) : Int\n  a + 1\nend\n\nfn main()"))
    items, digest = against(binary, unused, unused, 1)
    assert [r["primary"]["label"] for r in items if r["record"] == "diagnostic"] == [
        "Main.unused: arithmetic, `+` to `-`; no check runs this function",
        "Main.unused: int, `1` to `2`; no check runs this function"], items
    shorter(items[-1], digest, 2, 5, survived=2)

    good = "tests/cli/against/base/main.mo"
    assert run(binary, ["test", good, "--against", "tests/cli/against/none.mo"], 2).stderr == \
        b"--against names no file\n"
    refused = (b"--against runs every check and tries the mutants of what changed; it takes no "
               b"--only, --in, --calls, --seconds, --max-stack, --max-depth, --max-heap, --list, "
               b"--bundle or --strength: \"--against\"\n")
    for flags in (["--only", "Main.test.1"], ["--in", "Main"], ["--calls", "10"],
                  ["--seconds", "5"], ["--max-stack", "1MiB"], ["--max-depth", "100"],
                  ["--max-heap", "1GiB"],
                  ["--list"], ["--bundle"], ["--strength", "x"]):
        assert run(binary, ["test", good, "--against", good, *flags], 2).stderr == refused, flags
    items, _ = against(binary, "module Main\n\nfn main()\n  x\nend\n", base, 1)
    assert [r["record"] for r in items] == ["version", "diagnostic", "totals"], items
    assert items[1]["headline"] == "the --against program does not check", items
    assert items[1]["primary"]["label"].startswith("E0050: "), items
    assert items[-1]["gates"] == "did not run", items

    with tempfile.TemporaryDirectory() as scratch:
        old = pathlib.Path(scratch) / "lib"
        shutil.copytree(root / "lib", old)
        before = snapshot(old)
        args = ["test", "lib/list.mo", "--against", f"{old}/list.mo", "--json"]
        items = records(run(binary, args, 0), "test")
        assert snapshot(old) == before
        files = [p.relative_to(old).as_posix() for p in old.rglob("*.mo")]
        shorter(items[-1], bundle_digest(old, "list.mo", files), 0, 10)


def changes(items):
    return [(r["change"], r["name"], *([r["reason"]] if "reason" in r else []))
            for r in items if r["record"] == "change"]


def summary(items, passing, **counts):
    record = next(r for r in items if r["record"] == "changes")
    expected = {"no_longer_true": 0, "newly_true": 0, "about_something_new": 0, "not_compared": 0,
                **counts}
    assert {k: record[k] for k in expected} == expected and record["passing"] == passing, record
    assert items.index(record) < len(items) - 1 and all(
        r["record"] != "diagnostic" for r in items[:items.index(record)]), items


def change_summary(binary):
    base = BASE.read_text()
    buggy = edit(base, ("  a + b\n", "  a.abs + b\n"))
    fixed = edit(base, ("    add(2, 3) == 5\n", "    add(2, 3) == 5\n    add(-2, 3) == 1\n"))
    items, _ = against(binary, buggy, fixed, 0)
    assert changes(items) == [("newly true", "Main.add.example.2")], items
    summary(items, 8, newly_true=1)

    stricter = edit(base, ("    not small(2, 2)\n", "    small(2, 2)\n    not small(3, 2)\n"),
                    ("  a < b\n", "  a <= b\n"))
    items, _ = against(binary, base, stricter, 0)
    assert changes(items) == [("no longer true", "Main.small.example.2"),
                              ("newly true", "Main.small.example.2")], items
    assert items[-1]["gates"] == "ran shorter", items
    summary(items, 8, no_longer_true=1, newly_true=1)

    renamed = edit(base, ("fn twice(n", "fn double(n"), ("six?(twice(3))", "six?(double(3))"),
                   ("twice(0)", "double(0)"),
                   ("fn main()", TRIPLE))
    items, _ = against(binary, base, renamed, 0)
    unseen = "E0050: a name isn't visible in this block"
    assert changes(items) == [("could not be compared", "Main.test.1", unseen),
                              ("could not be compared", "Main.test.2", unseen),
                              ("about something new", "Main.test.1"),
                              ("about something new", "Main.test.2"),
                              ("about something new", "Main.triple.example.1")], items
    summary(items, 6, about_something_new=3, not_compared=2)

    behaviour = edit(renamed, ("  n * 2\n", "  n * 3\n"), ("six?(double(3))", "six?(double(2))"))
    items, _ = against(binary, base, behaviour, 0)
    assert {c[0] for c in changes(items)} == {"could not be compared", "about something new"}

    both = edit(stricter, ("fn twice(n", "fn double(n"), ("six?(twice(3))", "six?(double(3))"),
                ("twice(0)", "double(0)"))
    items, _ = against(binary, base, both, 0)
    assert changes(items)[:3] == [("no longer true", "Main.small.example.2"),
                                  ("could not be compared", "Main.test.1", unseen),
                                  ("could not be compared", "Main.test.2", unseen)], items

    failing = edit(base, ("  a + b\n", "  a + b + 1\n"))
    items, _ = against(binary, failing, base, 0)
    assert changes(items) == [("newly true", "Main.add.example.1")], items
    more = edit(base, ("    add(2, 3) == 5\n", "    add(2, 3) == 5\n    add(1, 1) == 2\n"))
    items, _ = against(binary, base, more, 0)
    assert changes(items) == [], items
    summary(items, 9)

    items, _ = against(binary, base, edit(base, ("  a + b\n", "  a - b\n")), 1)
    assert changes(items) == [("no longer true", "Main.add.example.1")], items
    assert items[-1]["gates"] == "did not run" and items[-1]["failed"] == 1, items
    worse = edit(failing, ("    add(2, 3) == 5\n", "    add(2, 3) == 5\n    add(0, 0) == 0\n"))
    items, _ = against(binary, failing, worse, 1)
    assert changes(items) == [], items
    summary(items, 7)

    helper = edit(base, ("fn six?(n: Int) : Bool\n  n == 6\n", "fn six?(_n: Int) : Bool\n  true\n"))
    items, _ = against(binary, base, helper, 1)
    assert changes(items) == [("could not be compared", "Main.test.1",
                               "it reads Main.six?, which changed")], items

    tools = (root / GATES / "tools/main.mo").read_text()
    without = edit(tools, ('  promise "twice doubles the count" on twice\n'
                           '    after == before * 2\n'
                           '    allows before: 2, reply: Unit, after: 4, now: t0\n'
                           '    forbids before: 2, reply: Unit, after: 3, now: t0\n  end\n', ""))
    items, _ = against(binary, tools, edit(without, ("(state * 2, Unit)", "(state * 3, Unit)")), 1)
    assert changes(items) == [("no longer true", "Main.service.promise.3")], items
    items, _ = against(binary, tools, without, 1)
    assert changes(items) == [], items
    summary(items, 5)

    overloads = (root / GATES / "intent/main.mo").read_text()
    example = "  examples\n    size(Box(width: 2)) == 2\n  end\n\n"
    items, _ = against(binary, overloads, edit(overloads, (example, "")), 1)
    assert changes(items) == [], items
    summary(items, 2)

    result, digest = against(binary, base, stricter, 0, json_output=False)
    lines = result.stdout.decode().splitlines()
    assert lines[-6:-3] == [
        "change: no longer true: Main.small.example.2", "change: newly true: Main.small.example.2",
        f"changes against previous version {digest}: 1 no longer true, 1 newly true, "
        "0 about something new, 0 could not be compared, 8 pass on both"], lines
