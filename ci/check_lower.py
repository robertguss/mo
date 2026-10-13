#!/usr/bin/env python3
"""Run lowered contract fixtures against independent expected outcomes and Lean.

Before the staged checker merges these are public mo_program fixtures, not
source inference substitutes. VM refusals are reported separately; --require-vm
is used with the join-capable integration build and allows no refusals.
"""

import json
import pathlib
import subprocess
import sys
import tempfile

from check_core_check import VERSION, WORLD, parse
from check_vm import oracle, run


def main():
    require_vm = "--require-vm" in sys.argv
    drivers = [pathlib.Path(arg) for arg in sys.argv[1:] if arg != "--require-vm"]
    assert drivers
    golden = None
    checked = matched = refused = host_entries = 0
    with tempfile.TemporaryDirectory(prefix="mo-lower-") as directory:
        root = pathlib.Path(directory)
        world = root / "world.core"
        world.write_text(f"(core {VERSION} {WORLD})\n")
        for index, driver in enumerate(drivers):
            corpus = root / str(index)
            corpus.mkdir()
            result = subprocess.run([str(driver), "--corpus", str(corpus)],
                                    capture_output=True, text=True, timeout=30)
            assert result.returncode == 0, (driver, result.stdout, result.stderr)
            documents = {path.name: path.read_bytes() for path in corpus.glob("*.core")}
            assert documents
            if golden is None:
                golden = documents
            else:
                assert documents == golden, f"nondeterministic lowering: {driver}"
            vm = driver.with_name("test_vm.test")
            for path in sorted(corpus.glob("*.core")):
                lines = path.with_suffix(".want").read_text().splitlines()
                expected = lines[:2]
                actual = oracle(path, world)
                assert actual == expected, (path.name, expected, actual)
                if len(lines) == 3:
                    report = subprocess.run(["formal/.lake/build/bin/mocore", str(path), str(world)],
                                            capture_output=True, text=True, timeout=15)
                    assert report.returncode in (0, 1, 2), report
                    effects = parse(report.stdout)[2][-1]
                    assert effects[0] == "effects"
                    assert effects[2] == json.loads(lines[2])["stderr"], (path.name, effects)
                checked += 1
                program = parse(path.read_text())[2]
                entry = next(function for function in program[2] if function[1] == program[4])
                if entry[2]:
                    # test_vm's public fixture runner takes zero arguments. The
                    # host's mo_call_main owns construction of runtime capabilities.
                    host_entries += 1
                    assert not require_vm, (path.name, "needs the capability-entry host runner")
                    continue
                actual = run(str(vm), path)
                if actual is None:
                    refused += 1
                    assert path.name in {"branch-false.core", "branch-true.core", "loops.core"}, (
                        vm, path.name, "unexpected compiler/host refusal")
                    assert not require_vm, (vm, path.name, "compiler or host refused")
                else:
                    assert actual == expected, (vm, path.name, expected, actual)
                    matched += 1
    print(f"lower: {checked} expected outcomes match Lean; "
          f"{matched} VM outcomes match; {refused} explicit compiler/host refusals; "
          f"{host_entries} capability entries need the host runner")


if __name__ == "__main__":
    main()
