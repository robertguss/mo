#!/usr/bin/env python3
"""Run lowered contract fixtures against independent expected outcomes and Lean.

These public mo_program fixtures cover contracts not yet produced by checking.
VM refusals are reported separately; --require-vm is used with the join-capable
integration build and allows no refusals. The host runner supplies capabilities.
"""

import json
import pathlib
import subprocess
import sys
import tempfile

from check_core_check import VERSION, WORLD, parse
from check_vm import oracle


def main():
    require_vm = "--require-vm" in sys.argv
    drivers = [pathlib.Path(arg) for arg in sys.argv[1:] if arg != "--require-vm"]
    assert drivers
    golden = None
    checked = matched = refused = 0
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
            vm = driver.parent.parent / "lower-source"
            for path in sorted(corpus.glob("*.core")):
                lines = path.with_suffix(".want").read_text().splitlines()
                expected = lines[:2]
                stderr = json.loads(lines[2])["stderr"] if len(lines) == 3 else []
                actual = oracle(path, world)
                assert actual == expected, (path.name, expected, actual)
                if len(lines) == 3:
                    report = subprocess.run(["formal/.lake/build/bin/mocore", str(path), str(world)],
                                            capture_output=True, text=True, timeout=15)
                    assert report.returncode in (0, 1, 2), report
                    effects = parse(report.stdout)[2][-1]
                    assert effects[0] == "effects"
                    assert effects[2] == stderr, (path.name, effects)
                checked += 1
                report = subprocess.run([str(vm), "--core", str(path)],
                                        capture_output=True, text=True, timeout=15)
                if report.returncode == 77:
                    refused += 1
                    assert path.name in {"branch-false.core", "branch-true.core", "loops.core"}, (
                        vm, path.name, "unexpected compiler/host refusal")
                    assert not require_vm, (vm, path.name, "compiler or host refused")
                else:
                    assert report.returncode == 0, (vm, path.name, report.stderr)
                    actual = report.stdout.rstrip("\n").split("\n", 1)
                    assert actual == expected, (vm, path.name, expected, actual)
                    assert report.stderr == "".join(s + "\n" for s in stderr), (
                        vm, path.name, stderr, report.stderr)
                    matched += 1
    print(f"lower: {checked} expected outcomes match Lean; "
          f"{matched} VM outcomes match; {refused} explicit compiler/host refusals")


if __name__ == "__main__":
    main()
