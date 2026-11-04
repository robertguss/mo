#!/usr/bin/env python3
"""Grade a Phase 4 session, and check that every scenario variant is sound.

  python3 learnability/phase4/grade.py session VARIANT MO FOLDER [STREAM KEY]
        grade the folder a session left. A defect variant is graded on the hidden tests and
        the queue's own tests; a question variant on the answer in the client's event stream
        STREAM, compared with the round's key file KEY (the key.json fixture.py build wrote),
        and on the queue's own tests. Prints one JSON object; exit 0 for a success, 1 for a
        failure.
  python3 learnability/phase4/grade.py --check MO ROOT [BUILT]
        every variant under ROOT/variants: SHA256SUMS match, the variant checks, is formatted
        and passes its own tests, its report names no internals, its prelude is what its
        fixture renders to; for a defect variant the reference fix passes and the unfixed
        variant and the broken fix each fail a hidden test; for a question variant, when BUILT
        holds the round's built folders (BUILT/NAME/data and BUILT/NAME/key.json), the key
        recomputed from the folder matches
  python3 learnability/phase4/grade.py --freeze VARIANT
        write VARIANT/SHA256SUMS

A variant folder holds main.mo, job-queue.mo and queue-tests.mo (what a session's folder starts
as), report.md, fixture.json, fixture-steps.mo and SHA256SUMS. A defect variant also holds
hidden/hidden-tests.mo (module HiddenTests), hidden/reference/ and hidden/broken/, each a whole
session folder. MO is the `mo` that grades; for a session, the one the session used.

Grading copies the session's folder, then puts the variant's queue-tests.mo over the copy's, so
an edit to the queue's tests counts for nothing. job-queue.mo keeps the session's code, since
fixes go there, but its test, examples and promise blocks must be the variant's, unchanged. A
program whose tools or wire types changed fails because the overlaid tests no longer compile;
the compile error is the recorded reason.
"""

import concurrent.futures
import datetime
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fixture  # noqa: E402

SECONDS = 240  # per command: the queue's own tests take about a minute on a debug build
SESSION_FILES = ('main.mo', 'job-queue.mo', 'queue-tests.mo')
BLOCK = re.compile(r'^(\s*)(test "|examples\s*$|promise ")')
LEAK_WORDS = re.compile(r'\b(handler|function|expire|release|checked|shown\?|leased_job|put|'
                        r'JobQueue|QueueTests|Main)\b|mo-|\.mo\b', re.IGNORECASE)
ANSWER = re.compile(r'^\W*ANSWER:\s*(.*?)\W*$')


def run(command, cwd):
    try:
        return subprocess.run([str(part) for part in command], cwd=cwd, capture_output=True,
                              timeout=SECONDS)
    except subprocess.TimeoutExpired:
        return None


def records(result):
    found = []
    for line in result.stdout.decode(errors='replace').splitlines():
        try:
            found.append(json.loads(line))
        except ValueError:
            continue
    return found


def check_problems(work, mo):
    """`mo check` on the queue's tests, which reach every module of the program."""
    result = run([mo, 'check', '--json', 'queue-tests.mo'], work)
    if result is None:
        return [f'mo check did not finish within {SECONDS} seconds']
    found = [f'{r["code"]} {r["headline"]}  {(r.get("primary") or {}).get("file")}:'
             f'{(r.get("primary") or {}).get("line")}'
             for r in records(result) if r.get('record') == 'diagnostic']
    if result.returncode != 0 and not found:
        found.append(f'mo check exited {result.returncode} with no diagnostic')
    return found


def test_problems(work, mo, file, module=None):
    """Runs one file's tests; every result must pass. Returns (compiled, problems)."""
    command = [mo, 'test', '--json', file] + (['--in', module] if module else [])
    result = run(command, work)
    if result is None:
        return True, [f'{file}: did not finish within {SECONDS} seconds']
    found = records(result)
    diagnostics = [f'{r["code"]} {r["headline"]}' for r in found
                   if r.get('record') == 'diagnostic']
    if diagnostics:
        return False, [f'{file} does not compile against this program: ' + '; '.join(diagnostics)]
    results = [r for r in found if r.get('record') == 'result']
    problems = [f'{r["name"]} {r["outcome"]}' + (f': {r["defect"].get("detail", "")}'
                                                 if r.get('defect') else '')
                for r in results if r.get('outcome') != 'passed']
    if result.returncode != 0 and not problems:
        problems.append(f'{file}: mo test exited {result.returncode}')
    if not results and not problems:
        problems.append(f'{file}: no test ran')
    return True, problems


def blocks(text):
    """The test, examples and promise blocks of a module, as text, in order."""
    lines, found, at = text.splitlines(), [], 0
    while at < len(lines):
        match = BLOCK.match(lines[at])
        if not match:
            at += 1
            continue
        indent, start = match.group(1), at
        at += 1
        while at < len(lines) and lines[at].rstrip() != f'{indent}end':
            at += 1
        found.append('\n'.join(line.rstrip() for line in lines[start:at + 1]))
        at += 1
    return found


def service_block(text):
    """main.mo's service declaration, from its `service` line to the `end` that closes it."""
    lines = [line.rstrip() for line in text.splitlines()]
    if 'service' not in lines:
        return None
    start = lines.index('service')
    end = lines.index('end', start) if 'end' in lines[start:] else len(lines)
    return lines[start:end + 1]


def prepared(variant, folder, work):
    """Copies the session's folder to work with the variant's tests over it. Returns notes and
    problems."""
    shutil.copytree(folder, work, symlinks=False)
    notes, problems = [], []
    frozen = (variant / 'queue-tests.mo').read_bytes()
    target = work / 'queue-tests.mo'
    if not target.exists() or target.is_symlink() or target.read_bytes() != frozen:
        notes.append("the session changed queue-tests.mo; the variant's copy was used")
    if target.is_symlink() or target.exists():
        target.unlink()
    target.write_bytes(frozen)
    session_queue = work / 'job-queue.mo'
    if not session_queue.is_file():
        problems.append('job-queue.mo is missing')
    elif blocks(session_queue.read_text(errors='replace')) != \
            blocks((variant / 'job-queue.mo').read_text()):
        problems.append("job-queue.mo's test, examples or promise blocks differ from the "
                        "variant's")
    session_main = work / 'main.mo'
    if not session_main.is_file():
        problems.append('main.mo is missing')
    elif service_block(session_main.read_text(errors='replace')) != \
            service_block((variant / 'main.mo').read_text()):
        problems.append("main.mo's service declaration differs from the variant's")
    return notes, problems


def defect(variant):
    return (variant / 'hidden' / 'hidden-tests.mo').exists()


def hidden_problems(variant, work, mo):
    for source in (variant / 'hidden' / 'hidden-tests.mo', variant / 'fixture-steps.mo'):
        target = work / source.name
        if target.is_symlink() or target.exists():
            target.unlink()
        shutil.copyfile(source, target)
    return test_problems(work, mo, 'hidden-tests.mo', 'HiddenTests')


def grade_folder(variant, mo, folder, answer_problems=None):
    """Returns (success, reasons, notes) for a folder, hidden tests and queue tests."""
    variant = pathlib.Path(variant)
    with tempfile.TemporaryDirectory() as temporary:
        work = pathlib.Path(temporary) / 'work'
        notes, problems = prepared(variant, pathlib.Path(folder), work)
        problems += check_problems(work, mo)  # a changed tool's compile error is a reason too
        if not problems:
            _, failed = test_problems(work, mo, 'queue-tests.mo')
            problems += failed
        if not problems and defect(variant):
            _, failed = hidden_problems(variant, work, mo)
            problems += failed
    problems += answer_problems or []
    return not problems, problems, notes


def last_message(stream):
    """The text of the client's last assistant message, from Claude Code's stream-json or
    Codex's --json events."""
    claude, codex = [], None
    for line in pathlib.Path(stream).read_text(encoding='utf-8', errors='replace').splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if not isinstance(event, dict):
            continue
        if event.get('type') == 'assistant':
            message = event.get('message') or {}
            texts = [b.get('text', '') for b in message.get('content') or []
                     if isinstance(b, dict) and b.get('type') == 'text']
            if texts:
                ident = message.get('id')
                if claude and claude[-1][0] == ident and ident is not None:
                    claude[-1][1].extend(texts)
                else:
                    claude.append((ident, texts))
        item = event.get('item') or {}
        if event.get('type') == 'item.completed' and item.get('type') == 'agent_message':
            codex = item.get('text', '')
    if claude:
        return '\n'.join(claude[-1][1])
    return codex


def extracted(text):
    """The one value the message's ANSWER: lines give, or None."""
    values = set()
    for line in (text or '').splitlines():
        match = ANSWER.match(line.strip().strip('`*'))
        if match:
            values.add(' '.join(match.group(1).strip('`* ').split()))
    return values.pop() if len(values) == 1 else None


def normalized(rule, value):
    if rule == 'shared_payload_jobs':
        return str(int(value)) if re.fullmatch(r'\d+', value) else None
    parts = value.split()
    if len(parts) != 2:
        return None
    try:
        when = datetime.datetime.fromisoformat(parts[1].replace('Z', '+00:00'))
    except ValueError:
        return None
    if when.tzinfo is None:
        return None
    when = when.astimezone(datetime.timezone.utc).replace(microsecond=0)
    return f'{parts[0].strip("`").lower()} {when.strftime("%Y-%m-%dT%H:%M:%SZ")}'


def answer_problems(variant, stream, key):
    rule = fixture.load(variant)['key']['rule']
    expected = json.loads(pathlib.Path(key).read_text())['answer']
    given = extracted(last_message(stream))
    if given is None:
        return ['the last message gives no single ANSWER: value']
    if normalized(rule, given) != normalized(rule, expected):
        return [f'the answer {given!r} is wrong']
    return []


def sums(variant):
    files = sorted(p for p in pathlib.Path(variant).rglob('*')
                   if p.is_file() and p.name != 'SHA256SUMS')
    return ''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  '
                   f'{p.relative_to(variant).as_posix()}\n' for p in files)


def stale(variant):
    """The files SHA256SUMS names wrongly or leaves out."""
    path = variant / 'SHA256SUMS'
    want = dict(reversed(line.split('  ', 1)) for line in
                (path.read_text().splitlines() if path.exists() else []))
    have = dict(reversed(line.split('  ', 1)) for line in sums(variant).splitlines())
    return sorted(name for name in set(want) | set(have) if want.get(name) != have.get(name))


def report_problems(variant):
    text = (variant / 'report.md').read_text()
    problems = [f'report.md names {m.group(0)!r}' for m in LEAK_WORDS.finditer(text)]
    answers = [line for line in text.splitlines() if line.startswith('ANSWER:')]
    if (variant / 'fixture.json').exists() and fixture.load(variant).get('key'):
        if len(answers) != 1:
            problems.append(f'report.md has {len(answers)} ANSWER: form lines, not one')
    elif answers:
        problems.append('a defect report asks for an answer')
    return problems


def format_problems(variant, mo):
    problems = []
    with tempfile.TemporaryDirectory() as temporary:
        for path in sorted(variant.rglob('*.mo')):
            copy = pathlib.Path(temporary) / 'copy.mo'
            shutil.copyfile(path, copy)
            result = run([mo, 'format', copy], temporary)
            if result is None or result.returncode != 0 or copy.read_bytes() != path.read_bytes():
                problems.append(f'{path.relative_to(variant)} is not formatted')
    return problems


def must_fail(variant, work, mo, what):
    """A problem unless the hidden tests compile against work and at least one fails."""
    compiled, failed = hidden_problems(variant, work, mo)
    if compiled and any(line.startswith('HiddenTests.') for line in failed):
        return []
    return [f'{what} must compile and fail a hidden test: '
            + ('; '.join(failed) or 'every hidden test passed')]


def check_variant(variant, mo, built):
    variant = pathlib.Path(variant)
    problems = [f'SHA256SUMS is stale for {name}' for name in stale(variant)]
    problems += report_problems(variant)
    problems += format_problems(variant, mo)
    if not fixture.rendered(variant):
        problems.append('fixture-steps.mo is not what fixture.json renders to')
    with tempfile.TemporaryDirectory() as temporary:
        start = pathlib.Path(temporary) / 'start'
        start.mkdir()
        for name in SESSION_FILES:
            shutil.copyfile(variant / name, start / name)
        with tempfile.TemporaryDirectory() as second:
            work = pathlib.Path(second) / 'work'
            prepared(variant, start, work)
            problems += check_problems(work, mo)
            _, failed = test_problems(work, mo, 'queue-tests.mo')
            problems += [f'its own tests: {line}' for line in failed]
            if defect(variant):
                problems += must_fail(variant, work, mo, 'the unfixed variant')
        if defect(variant):
            success, reasons, _ = grade_folder(variant, mo, variant / 'hidden' / 'reference')
            if not success:
                problems += [f'the reference fix: {reason}' for reason in reasons]
            with tempfile.TemporaryDirectory() as third:
                work = pathlib.Path(third) / 'work'
                prepared(variant, variant / 'hidden' / 'broken', work)
                problems += must_fail(variant, work, mo, 'the broken fix')
    if not defect(variant) and built is not None:
        folder = pathlib.Path(built) / variant.name
        os.environ.setdefault('GRADE_KEY_TOKEN', os.urandom(16).hex())
        recomputed = fixture.key_from(variant, mo, folder / 'data', 'GRADE_KEY_TOKEN')
        recorded = json.loads((folder / 'key.json').read_text())
        if recomputed['answer'] != recorded['answer']:
            problems.append(f'the key from the built folder is {recomputed["answer"]!r}, but '
                            f'key.json says {recorded["answer"]!r}')
    return problems


def main(args):
    if len(args) in (3, 4) and args[0] == '--check':
        mo = pathlib.Path(args[1]).resolve()
        variants = sorted(p for p in (pathlib.Path(args[2]) / 'variants').iterdir()
                          if p.is_dir())
        if not variants:
            print(f'no variants under {pathlib.Path(args[2]) / "variants"}', file=sys.stderr)
            return 1
        built = args[3] if len(args) == 4 else None

        def checked(variant):
            if not (variant / 'fixture.json').exists():
                return ['no fixture.json, so this is not a variant']
            return check_variant(variant, mo, built)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(checked, variants))
        bad = 0
        for variant, problems in zip(variants, results):
            print(f'{"ok  " if not problems else "FAIL"} {variant.name}')
            for problem in problems:
                print('     ' + problem.replace('\n', '\n     '))
            bad += bool(problems)
        unkeyed = ', question keys not recomputed (no built folders given)'
        print(f'phase 4 variants: {len(variants) - bad} of {len(variants)} sound'
              + ('' if built is not None else unkeyed))
        return 0 if bad == 0 else 1
    if len(args) == 2 and args[0] == '--freeze':
        variant = pathlib.Path(args[1])
        (variant / 'SHA256SUMS').write_text(sums(variant))
        return 0
    if len(args) in (4, 6) and args[0] == 'session':
        variant = pathlib.Path(args[1])
        mo = pathlib.Path(args[2]).resolve()
        if defect(variant) != (len(args) == 4):
            print('a defect variant takes no stream or key; a question takes both',
                  file=sys.stderr)
            return 2
        answers = answer_problems(variant, args[4], args[5]) if len(args) == 6 else None
        success, reasons, notes = grade_folder(variant, mo, args[3], answers)
        print(json.dumps({'variant': variant.name, 'success': success, 'reasons': reasons,
                          'notes': notes}, indent=2))
        return 0 if success else 1
    print(__doc__.split('\n\n')[1], file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
