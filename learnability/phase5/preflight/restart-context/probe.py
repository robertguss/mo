#!/usr/bin/env python3
"""Isolate the recorded migration source's old-type dependency."""
import json
import pathlib
import subprocess
import sys
import tempfile

OLD = """module Main
struct State
  old_field: Int
end
service
  state State = State(old_field: 7)
  tools read
  token from env "TOKEN"
end
fn read(state: State, _now: Time) : Int
  state.old_field
end
"""
NEW = """module Main
use UpgradeOld
struct State
  new_field: Int
end
service
  state State = State(new_field: 7)
  tools read
  token from env "TOKEN"
end
fn read(state: State, _now: Time) : Int
  state.new_field
end
fn migrate(old: UpgradeOld.State) : State
  State(new_field: old.old_field)
end
"""
CONTEXT = """module UpgradeOld
expose State(..)
struct State
  FIELD: Int
end
"""


def run(binary, *args):
    result = subprocess.run([str(binary), *map(str, args)], capture_output=True, text=True)
    return {"exit": result.returncode, "stdout": result.stdout, "stderr": result.stderr}


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    evidence = {}
    with tempfile.TemporaryDirectory(prefix="mo-restart-context-") as temporary:
        root = pathlib.Path(temporary)
        old, candidate = root / "old", root / "candidate"
        old.mkdir()
        candidate.mkdir()
        (old / "main.mo").write_text(OLD)
        entry = candidate / "main.mo"
        entry.write_text(NEW)
        checked = run(binary, "check", entry, "--upgrade-from", old / "main.mo", "--migrate", "migrate", "--json")
        assert checked["exit"] == 0 and checked["stderr"] == "", checked
        assert json.loads(checked["stdout"])["status"] == "accepted", checked
        evidence["v2_typed_upgrade"] = checked
        for generation, source in (("v2", NEW), ("v3_tool_only", NEW.replace("state.new_field\n", "state.new_field + 1\n"))):
            entry.write_text(source)
            for field in ("old_field", "new_field"):
                (candidate / "upgrade-old.mo").write_text(CONTEXT.replace("FIELD", field))
                result = run(binary, "check", entry)
                if field == "old_field":
                    assert result["exit"] == 0 and result["stderr"] == "", result
                else:
                    assert result["exit"] == 1 and "error E0065:" in result["stderr"] and "old_field" in result["stderr"], result
                evidence[generation + "_context_" + field] = result
    print(json.dumps(evidence, indent=2))


if __name__ == "__main__":
    main()
