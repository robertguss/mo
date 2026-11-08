#!/usr/bin/env python3
"""Build the isolated transfer prototype against a completed gcc debug build."""
import json
import pathlib
import subprocess
import tempfile
from synthesize import verify

ROOT = pathlib.Path(__file__).resolve().parents[3]
objects = sorted(p for p in (ROOT / 'build/gcc/debug/src').rglob('*.o') if p.name != 'main.o')
assert objects
with tempfile.TemporaryDirectory(prefix='mo-transfer-') as temporary:
    folder = pathlib.Path(temporary)
    executable = folder / 'transfer'
    subprocess.run(['gcc', '-std=c17', '-g', '-Isrc', '-Itests', '-Ivendor/sqlite',
                    str(ROOT / 'learnability/phase5/preflight/transfer.c'),
                    *map(str, objects), 'build/gcc/debug/vendor/sqlite/sqlite3.o',
                    '-lm', '-pthread', '-o', str(executable)], cwd=ROOT, check=True)
    results = []
    for index in range(5):
        run = subprocess.run([str(executable), str(folder / f'store-{index}.sqlite')],
                             stdout=subprocess.PIPE, text=True, check=True, timeout=30)
        results.append(json.loads(run.stdout))
    synthesis = verify(executable, folder, ROOT / "build/gcc/debug/mo")
    print(json.dumps({"transfers": results, "synthesis": synthesis}, indent=2))
