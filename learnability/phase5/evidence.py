#!/usr/bin/env python3
"""Draft evaluation aggregation and exact tracked build-input comparison.

This does not collect sessions, attest hidden grading, or authorize collection.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess

INPUTS = ('src', 'lib', 'runtime', 'vendor', 'Makefile', 'ci/embed_docs.py',
          'ci/embed_protocol.py', 'docs/agent-guide.md', 'docs/reference/README.md')
ORDER = ('codex', 'claude', 'codex', 'claude')


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    freeze = sub.add_parser('build-inputs')
    freeze.add_argument('root', type=pathlib.Path)
    freeze.add_argument('--compare', type=pathlib.Path)
    grade = sub.add_parser('gate')
    grade.add_argument('records', type=pathlib.Path)
    args = parser.parse_args()
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
