#!/usr/bin/env python3
"""Copies a session's manifest and grade into a folder for this public repository.

  python3 learnability/phase4/publish.py OUT DEST

OUT is a session's record folder (harness.py run); DEST, which must not exist yet, gets
manifest.json and grade.json. The grader names each hidden test that failed, with its detail,
and the hidden tests stay private until the final round, so every such reason is replaced by
the one line HIDDEN. Nothing else is copied: the stream, the proxy log, the service log and the
folders stay in OUT, and the scanner (scan.py) still reads DEST before it is committed.
"""

import json
import pathlib
import sys

HIDDEN = 'a hidden test failed (named in the private notes)'
PRIVATE = ('HiddenTests', 'hidden-tests.mo', 'fixture-steps')


def redacted(grade):
    grader = grade.get('grader') or {}
    grader['reasons'] = [HIDDEN if any(p in reason for p in PRIVATE) else reason
                         for reason in grader.get('reasons') or []]
    return grade


def publish(out, dest):
    out, dest = pathlib.Path(out), pathlib.Path(dest)
    manifest = json.loads((out / 'manifest.json').read_text())
    grade = redacted(json.loads((out / 'grade.json').read_text()))
    dest.mkdir(parents=True, exist_ok=False)
    (dest / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (dest / 'grade.json').write_text(json.dumps(grade, indent=2) + '\n')


def main(args):
    if len(args) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        publish(*args)
    except (OSError, ValueError) as error:
        print(f'publish.py: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
