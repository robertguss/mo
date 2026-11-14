#!/usr/bin/env python3
"""Defect replays: `mo replay` and Lean's evaluator stop the same way on the same handler inputs.

    check_offline_core.py MO...

Each folder of tests/offline-core holds a service program, `main.mo`, and `case.txt`, the inputs
of one of its handlers written by hand as Core text: the state, the arrival time and the arguments,
with the stop Lean must give, a passing argument list and what Lean must return for it. For each
`mo` executable and each fixture:

- C: a real `mo serve` gets the MCP call that fails, is stopped, and `mo replay FILE --failure 1
  --data DIR --json` must report `reproduced` with a defect whose reason is Lean's stop. A copy of
  the record whose arguments are the passing ones must replay as `not_reproduced`.
- Lean: the handler's Core, from `mo check --emit=core`, becomes the entry of a Run applied to the
  hand-written inputs; `mocore` must stop with the fixture's stop, and return the fixture's value
  for the passing arguments.

The inputs reach Lean as the author wrote them, not through a C value printer. A `stop` signature
names the reason `stop` without the program's text, so Lean's message is checked against the
fixture only. Lean reports no location; spans are compared only by `mo replay`, recorded against
offline. The handlers do not read the time, so the arrival the author wrote need not equal the one
the service recorded. Only the defect class is compared; the others are listed as excluded.

    MO_MOCORE   the Lean oracle (default formal/.lake/build/bin/mocore)
"""

import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from cli_common import records  # noqa: E402
from check_core_check import VERSION, WORLD, parse  # noqa: E402
from check_offline import edit_record, serve  # noqa: E402
from check_vm import outcome_of  # noqa: E402

root = pathlib.Path(__file__).resolve().parent.parent
FIXTURES = root / "tests" / "offline-core"
FUEL = 10000
FIELDS = ("handler", "call", "state", "arrival", "arguments", "stop", "passing", "returns")
# The failure classes a replay can report that this comparison leaves out, and why.
EXCLUDED = {
    "work": "Lean's fuel bounds evaluation depth, not VM instructions",
    "memory": "Lean has no heap, stack or call-depth limit",
    "state_size": "Lean has no saved encoding of the new state",
}
# An Int argument's saved bytes: MOSV version 1, the schema's length, the Int schema (tag 3, no
# type parameters, one group, no fields), then the value's eight signed bytes.
INT_SCHEMA = bytes([3]) + (0).to_bytes(4, "little") + (1).to_bytes(4, "little") + (0).to_bytes(4, "little")
INT_PREFIX = b"MOSV\x01" + len(INT_SCHEMA).to_bytes(4, "little") + INT_SCHEMA


def fixture(folder):
    case = {}
    for line in (folder / "case.txt").read_text().splitlines():
        if line and not line.startswith("#"):
            key, _, value = line.partition(": ")
            assert key in FIELDS and key not in case, (folder, line)
            case[key] = value
    assert set(case) == set(FIELDS), (folder, sorted(set(FIELDS) - set(case)))
    case["call"] = json.loads(case["call"])
    case["stop"] = parse(case["stop"])
    return case


def program(binary, source, handler):
    """The checked program's Core, with the handler as its entry."""
    result = subprocess.run([str(binary), "check", "--emit=core", str(source)], capture_output=True,
                            text=True, timeout=120)
    assert result.returncode == 0 and not result.stderr, (source, result.stderr)
    text = result.stdout.rstrip("\n")
    prefix, entry = f"(core {VERSION} ", ' "%entry" (Metadata '
    assert text.startswith(prefix + "(Program ") and text.endswith(")"), text[:200]
    assert text.count(entry) == 1, (source, "the service program's entry is not %entry")
    name = f"Main.{handler}"
    assert f'(FunDecl "{name}" ' in text, (source, name)
    return text[len(prefix):-1].replace(entry, f' "{name}" (Metadata ')


def lean(oracle, folder, core, inputs):
    path = folder / "run.core"
    path.write_text(f"(core {VERSION} (Run {core} {WORLD} (entry ({inputs})) {FUEL}))\n")
    result = subprocess.run([oracle, str(path)], capture_output=True, text=True, timeout=300)
    assert result.returncode in (0, 1), (path, result.returncode, result.stdout[:300], result.stderr)
    return result.stdout


def reason(stdout):
    """Lean's stop in the spelling of a replay signature's reason."""
    bug = parse(stdout)[2][1]
    return "stop" if bug[0] == "stop" else outcome_of(stdout)[1]


def replay(binary, file, data, status):
    result = subprocess.run([str(binary), "replay", "--json", str(file), "--failure", "1", "--data",
                             str(data)], capture_output=True, timeout=120)
    assert result.returncode == status, (data, result.returncode, result.stdout, result.stderr)
    items = records(result, "replay")
    assert [item["record"] for item in items] == ["version", "original", "totals"], items
    return items[1]


def with_arguments(values):
    """A change to a failure message giving it these Int arguments in place of its own."""
    assert re.fullmatch(r"\(int -?\d+\)(?: \(int -?\d+\))*", values), values
    numbers = [int(n) for n in re.findall(r"\(int (-?\d+)\)", values)]

    def change(fields):
        assert len(fields["arguments"]) == len(numbers), (fields["arguments"], numbers)
        for argument in fields["arguments"]:
            assert argument.startswith(INT_PREFIX) and len(argument) == len(INT_PREFIX) + 8, argument
        fields["arguments"] = [INT_PREFIX + n.to_bytes(8, "little", signed=True) for n in numbers]
    return change


def check(binary, oracle, scratch):
    """Every fixture against one mo executable; SCRATCH is a fresh folder."""
    covered = []
    for folder in sorted(p for p in FIXTURES.iterdir() if p.is_dir()):
        case, work = fixture(folder), scratch / folder.name
        work.mkdir(parents=True)
        source = (folder / "main.mo").read_text()
        core = program(binary, folder / "main.mo", case["handler"])
        inputs = f"{case['state']} {case['arrival']}"

        stopped = lean(oracle, work, core, f"{inputs} {case['arguments']}")
        report = parse(stopped)[2]
        assert report[0] == "stopped" and report[1] == case["stop"], (folder, stopped)
        returned = lean(oracle, work, core, f"{inputs} {case['passing']}")
        report = parse(returned)[2]
        assert report[0] == "returned" and report[1] == case["returns"], (folder, returned)

        data = serve(binary, work, source, [(case["handler"], case["call"])])
        original = replay(binary, work / "main.mo", data, 0)
        assert original["handler"] == case["handler"] and original["outcome"] == "reproduced", original
        run = original["run"]
        assert run["status"] == "defect" and run["signature"]["class"] == "defect", run
        assert run["signature"]["reason"] == reason(stopped), (folder, run["signature"], stopped)
        passing = edit_record(data, work, "passing", 1, with_arguments(case["passing"]))
        other = replay(binary, work / "main.mo", passing, 1)
        assert other["outcome"] == "not_reproduced" and other["run"]["status"] == "succeeded", other
        covered.append(case["stop"][0])
        print(f"  {folder.name}: {case['handler']} stops with {run['signature']['reason']!r} in mo "
              f"replay and Lean; the passing arguments return {case['returns']} in Lean and are "
              "not_reproduced in mo replay")
    missing = {"stop", "overflow", "divByZero"} - set(covered)
    assert not missing, f"no fixture stops with {sorted(missing)}"
    return len(covered)


def main():
    binaries = [pathlib.Path(b).resolve() for b in sys.argv[1:]]
    if not binaries:
        raise AssertionError("at least one built mo executable is required")
    oracle = pathlib.Path(os.environ.get("MO_MOCORE", root / "formal/.lake/build/bin/mocore"))
    if not oracle.exists():
        raise AssertionError(f"{oracle} is not built; run `make formal` first")
    with tempfile.TemporaryDirectory(prefix="mo-offline-core-") as directory:
        for index, binary in enumerate(binaries):
            print(f"{binary}:")
            count = check(binary, str(oracle.resolve()), pathlib.Path(directory) / str(index))
            print(f"{binary}: {count} defect replays agree with Lean on the stop reason")
    print("excluded failure classes: " + "; ".join(f"{k} ({v})" for k, v in EXCLUDED.items()))


if __name__ == "__main__":
    main()
