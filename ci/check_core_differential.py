#!/usr/bin/env python3
"""Generated Core programs (ci/gen_core.py) must give the same outcome on the C VM and in Lean.

    check_core_differential.py TEST_VM_DRIVER...

Each driver is a built tests/test_vm.test. Every program is compiled, loaded and run by the driver
and evaluated by Lean's `mocore`; the two reports must be equal, stop messages included. Only
programs inside lowering's representation contract are generated (gen_core `lowerable`), because
codegen refuses the rest by design. A program either engine cuts off at its budget (the VM's
instruction limit, Lean's fuel) is counted, not compared; one engine alone doing so is capped at 2%.

    MO_CORE_DIFF            program count (default 1000)
    MO_CORE_DIFF_SEED       first seed (default 0); the nightly job gives each shard its own range
    MO_CORE_DIFF_MISMATCHES stop after this many mismatches (default 5)
    MO_CORE_DIFF_OUT        a folder to keep every mismatching program in, as SEED.core
    MO_CORE_DIFF_CORPUS     a folder of .core programs compared first (default tests/differential)
    MO_MOCORE               the Lean oracle (default formal/.lake/build/bin/mocore)

A program a run found to differ is minimized by hand, fixed in whichever engine was wrong, and
committed to tests/differential/ as NAME.core (a bare `(core 0.8 (Program ...))`), so every later
run checks it. Seeds make every run reproducible: rerun with MO_CORE_DIFF=1 MO_CORE_DIFF_SEED=<seed>. All storage
is disposable.
"""

import collections
import concurrent.futures
import os
import pathlib
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gen_core  # noqa: E402
from check_vm import canonical, outcome_of  # noqa: E402

RUN_PREFIX = f"(core {gen_core.VERSION} (Run "
RUN_SUFFIX = f" {gen_core.WORLD} (entry ()) 0))\n"
FUEL = 10000
BUDGET = "budget"


def split(seed):
    """The program text, bare for the VM driver, and the same program as a Run for Lean."""
    run = gen_core.document(seed, lowerable=True)
    assert run.startswith(RUN_PREFIX) and run.endswith(RUN_SUFFIX), run
    return f"(core {gen_core.VERSION} {run[len(RUN_PREFIX):-len(RUN_SUFFIX)]})\n", run


def vm(driver, path):
    try:
        return vm_run(driver, path)
    except subprocess.TimeoutExpired:
        return "vm timed out"


def vm_run(driver, path):
    # Bytes, not text mode: universal newlines would turn a returned "\r" into "\n".
    result = subprocess.run([driver, "--core", path], capture_output=True, timeout=60)
    if result.returncode != 0:
        return f"vm exit {result.returncode}: {result.stderr.decode('utf-8', 'replace').strip()[:300]}"
    kind, _, detail = result.stdout.decode("utf-8")[:-1].partition("\n")
    if "instruction limit" in detail or "instruction budget" in detail:
        return BUDGET
    return canonical([kind, detail])


def lean(oracle, path, world):
    try:
        return lean_run(oracle, path, world)
    except subprocess.TimeoutExpired:
        return "mocore timed out"


def lean_run(oracle, path, world):
    result = subprocess.run([oracle, path, world, "--fuel", str(FUEL)], capture_output=True,
                            text=True, timeout=60)
    if result.returncode == 3:
        return BUDGET
    if result.returncode not in (0, 1, 2):
        return f"mocore exit {result.returncode}: {result.stdout.strip()[:300]}"
    try:
        return outcome_of(result.stdout)
    except Exception as error:  # a report this script cannot read is a finding, not a crash
        return f"unreadable mocore report ({error!r}): {result.stdout.strip()[:300]}"


def main():
    started = time.monotonic()
    drivers = sys.argv[1:]
    if not drivers:
        raise AssertionError("at least one built test_vm driver is required")
    count = int(os.environ.get("MO_CORE_DIFF", "1000"))
    first = int(os.environ.get("MO_CORE_DIFF_SEED", "0"))
    limit = int(os.environ.get("MO_CORE_DIFF_MISMATCHES", "5"))
    keep = os.environ.get("MO_CORE_DIFF_OUT")
    oracle = str(pathlib.Path(os.environ.get("MO_MOCORE", "formal/.lake/build/bin/mocore")).resolve())
    if not pathlib.Path(oracle).exists():
        raise AssertionError(f"{oracle} is not built; run `make formal` first")
    if count < 1:
        raise AssertionError("MO_CORE_DIFF must be at least 1")
    with tempfile.TemporaryDirectory(prefix="mo-core-diff-") as tmp:
        root = pathlib.Path(tmp)
        world = root / "world.core"
        world.write_text(f"(core {gen_core.VERSION} {gen_core.WORLD})\n")
        tally = collections.Counter()
        mismatches = []

        def compare(name, bare):
            path = root / f"{name}.core"
            path.write_text(bare)
            want = lean(oracle, str(path), str(world))
            return name, bare, want, {d: vm(d, str(path)) for d in drivers}

        def one(seed):
            return compare(seed, split(seed)[0])

        corpus = sorted(pathlib.Path(os.environ.get("MO_CORE_DIFF_CORPUS", "tests/differential")).glob("*.core"))
        for name, bare, want, got in (compare(f.stem, f.read_text()) for f in corpus):
            for driver, outcome in got.items():
                if outcome != want or not isinstance(want, list):
                    mismatches.append((name, driver, want, outcome, bare))
        tally["corpus"] = len(corpus)

        with concurrent.futures.ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 2)) as pool:
            for seed, bare, want, got in pool.map(one, range(first, first + count)):
                for driver, outcome in got.items():
                    if want == BUDGET and outcome == BUDGET:
                        tally[BUDGET] += 1
                    elif want == BUDGET or outcome == BUDGET:
                        tally["one engine out of budget"] += 1
                    elif outcome == want and isinstance(want, list):
                        tally[want[0]] += 1
                    else:
                        mismatches.append((seed, driver, want, outcome, bare))
                if len({m[0] for m in mismatches}) >= limit:
                    pool.shutdown(wait=False, cancel_futures=True)
                    break
    for seed, driver, want, got, bare in mismatches:
        print(f"MISMATCH seed {seed} ({driver})\n  lean: {want}\n  vm:   {got}\n{bare}", file=sys.stderr)
        if keep:
            pathlib.Path(keep).mkdir(parents=True, exist_ok=True)
            (pathlib.Path(keep) / f"{seed}.core").write_text(bare)
    if mismatches:
        raise AssertionError(f"{len(mismatches)} generated programs differ between the VM and Lean")
    one_sided = tally["one engine out of budget"]
    if one_sided > 0.02 * count * len(drivers):
        raise AssertionError(f"one engine alone ran out of budget on {one_sided} runs: {dict(tally)}")
    compared = sum(tally[k] for k in tally if k not in (BUDGET, "corpus", "one engine out of budget"))
    if compared < 0.9 * count * len(drivers):
        raise AssertionError(f"only {compared} of {count * len(drivers)} runs were compared: {dict(tally)}")
    if not tally["returned"] or not tally["stopped"]:
        raise AssertionError(f"the programs never both returned and stopped: {dict(tally)}")
    print(f"Core differential: {count} programs (seeds {first}..{first + count - 1}) give the same outcome "
          f"in Lean and in {len(drivers)} VM build(s): {dict(tally)}; {time.monotonic() - started:.1f}s")


if __name__ == "__main__":
    main()
