#!/usr/bin/env python3
"""Evaluation gate, tracked build-input comparison and the collection freeze manifest.

This does not collect sessions, attest hidden grading, or authorize collection.
freeze FOLDER hashes the repository's protocol, harness, grader, fixtures, build
inputs, drill and pause instruments and every binary the collection and the
measurements run, plus these operator files in FOLDER: spec.json, controls.json,
verification.json, build-commands.json, compiler-versions.json and any files under
FOLDER/controls/ and FOLDER/verification/. It writes FOLDER/manifest.json.
"""
import argparse
import datetime
import hashlib
import json
import pathlib
import subprocess

INPUTS = ('src', 'lib', 'runtime', 'vendor', 'Makefile', 'ci/embed_docs.py',
          'ci/embed_protocol.py', 'docs/agent-guide.md', 'docs/reference/README.md')
ORDER = ('codex', 'claude', 'codex', 'claude')
ROOT = pathlib.Path(__file__).resolve().parents[2]
CLIENTS = ('mo', 'codex', 'claude')
INSTRUMENTS = ('ci/check.sh', 'ci/check_kill_loop.py', 'ci/check_kill_stages.py',
               'tests/kill-loop/migration.mo.inc', 'learnability/phase5/measure_pause.py',
               'learnability/phase5/compiler_load.py')


def evaluation_gate(rows, scan_clean):
    valid = len(rows) == len(ORDER)
    by_slot = {}
    for row in rows:
        slot = row.get('slot')
        if type(slot) is not int or not 1 <= slot <= len(ORDER) or slot in by_slot:
            valid = False
            continue
        by_slot[slot] = row
        if row.get('client') != ORDER[slot - 1] or row.get('development', False):
            valid = False
    counts = {client: sum(row.get('correct') is True and not row.get('development', False)
                          for row in by_slot.values() if row.get('client') == client)
              for client in set(ORDER)}
    passed = valid and scan_clean is True and sum(counts.values()) >= 3 \
        and all(count >= 1 for count in counts.values())
    return {'passed': passed, 'phase': 'open', 'correct': counts, 'denominator': 4,
            'complete_valid_slots': valid, 'publication_scan_clean': scan_clean,
            'release_authorized': False}


def build_inputs(root):
    root = pathlib.Path(root)
    names = subprocess.check_output(
        ['git', '-C', str(root), 'ls-files', '-z', '--', *INPUTS]).decode().split('\0')
    return {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
            for name in sorted(filter(None, names))}


def digests(paths):
    result = {}
    for path in paths:
        if not path.exists():
            raise FileNotFoundError(str(path))
        for item in [path] if path.is_file() else sorted(path.rglob('*')):
            if item.is_file():
                result[str(item.resolve())] = hashlib.sha256(item.read_bytes()).hexdigest()
    return result


def freeze(root, folder, experiment, installed, debug, optimized):
    root, folder = pathlib.Path(root).resolve(), pathlib.Path(folder).resolve()
    spec = json.loads((folder / 'spec.json').read_text())
    inputs = build_inputs(root)
    (folder / 'build-inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    evaluation, phase4 = root / 'learnability/phase5/evaluation', root / 'learnability/phase4'
    extra = lambda name: [folder / name] if (folder / name).is_dir() else []
    artifacts = {
        'protocol': digests([root / 'learnability/phase5/protocol.md', root / 'learnability/phase5/collection.md',
                             root / 'learnability/phase5/results' / experiment / 'declaration.json']),
        'prompt': digests([pathlib.Path(spec['prompt'])]),
        'harness': digests([*evaluation.glob('*.py'), *phase4.glob('*.py')]),
        'proxy': digests([evaluation / 'proxy.py', phase4 / 'proxy.py']),
        'scanner': digests([phase4 / 'scan.py', evaluation / 'publish.py']),
        'grader': digests([evaluation / 'grade.py', evaluation / 'protected_regrade.py',
                           root / 'learnability/phase5/evidence.py']),
        'fixtures': digests([*(pathlib.Path(spec[k]) for k in ('source', 'context', 'data', 'expected')),
                             folder / 'spec.json', root / 'services/task-queue', evaluation / 'fixtures']),
        'controls': digests([folder / 'controls.json', *extra('controls')]),
        'build_inputs': digests([*(root / name for name in inputs), folder / 'build-inputs.json']),
        'build_commands': digests([folder / 'build-commands.json']),
        'compiler_versions': digests([folder / 'compiler-versions.json']),
        'verification': digests([folder / 'verification.json', *extra('verification')]),
        'instruments': digests([root / name for name in INSTRUMENTS]),
        'binaries': digests([*(pathlib.Path(installed) / name for name in CLIENTS),
                             pathlib.Path(debug), pathlib.Path(optimized)]),
    }
    commit = lambda ref: subprocess.check_output(['git', '-C', str(root), 'rev-parse', ref], text=True).strip()
    main = commit('refs/remotes/origin/main')
    manifest = dict(created_at=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    repository=str(root), experiment=experiment, repair_commit=main, main_commit=main,
                    candidate_commit=commit('HEAD'), slots=list(ORDER),
                    verification_evidence=str(folder / 'verification.json'),
                    controls_evidence=str(folder / 'controls.json'),
                    binaries={name: hashlib.sha256((pathlib.Path(installed) / name).read_bytes()).hexdigest()
                              for name in CLIENTS},
                    artifacts=artifacts)
    (folder / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    freeze = sub.add_parser('build-inputs')
    freeze.add_argument('root', type=pathlib.Path)
    freeze.add_argument('--compare', type=pathlib.Path)
    grade = sub.add_parser('gate')
    grade.add_argument('records', type=pathlib.Path)
    frozen = sub.add_parser('freeze')
    frozen.add_argument('folder', type=pathlib.Path)
    frozen.add_argument('--experiment', required=True)
    frozen.add_argument('--debug', type=pathlib.Path, required=True, help='debug pause measurement binary')
    frozen.add_argument('--optimized', type=pathlib.Path, required=True, help='optimized pause measurement binary')
    frozen.add_argument('--installed', type=pathlib.Path, default=pathlib.Path('/opt/mo-phase5/bin'))
    args = parser.parse_args()
    if args.command == 'freeze':
        manifest = freeze(ROOT, args.folder, args.experiment, args.installed, args.debug, args.optimized)
        print('frozen artifacts:', sum(map(len, manifest['artifacts'].values())))
        return 0
    if args.command == 'build-inputs':
        result = build_inputs(args.root)
        if args.compare:
            old = json.loads(args.compare.read_text())
            changes = sorted(key for key in old.keys() | result.keys()
                             if old.get(key) != result.get(key))
            print(json.dumps({'matches': not changes, 'changed': changes}, indent=2))
            return bool(changes)
        print(json.dumps(result, indent=2))
        return 0
    record = json.loads(args.records.read_text())
    result = evaluation_gate(record['sessions'], record['publication_scan_clean'])
    print(json.dumps(result, indent=2))
    return not result['passed']


if __name__ == '__main__':
    raise SystemExit(main())
