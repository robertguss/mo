#!/usr/bin/env python3
"""The review door's proof on the task queue: planted changes and frozen wrong versions pushed
through a real mo serve, mo push and mo review under both review settings.

The task queue is served from services/task-queue on a fresh data folder, every item of the
served program is approved with `mo review --base ... --approve ...`, and that state is copied
once per review setting. Each case is pushed to its own copy of that state:

- a planted `intent` change, which edits or removes an approved item, is held under both settings;
- a planted `fix`, which keeps every approved item and every owner called, activates under
  `intent` and is held under `everything`, where its decision row says `intent` would have
  activated it;
- a counted wrong version the strength record counts as killed is refused with E1433 under both
  settings. One it counts as survived passed every promise and example, but the door also runs
  the program's tests, so it is either refused by a test (E1433) or reconciled as a fix.

The classification the door computes for each case that passes its checks, whether `intent`
would have activated it, must equal its declared kind. Calls to the old program go on during
every push and must all be answered, and the active program must be the one each outcome leaves.
One held push is answered with `mo review --yes ID` and another with `--no ID`.

With --bypass, the same cases run against a build whose push decision activates every candidate
that passes its checks. That run must fail with exactly one mismatch for each push the door
held: the control that the proof can fail.

Usage: check_door.py MO PLANTED [--bypass MO] [--all] [--wrong SET RECORD]... [--jobs N]
[--keep]. PLANTED is a folder of pNN/ replacement files and their manifest.json. Without --all
it makes the short run in the drivers group: the first two planted changes of each kind and
three counted wrong versions; --all pushes every case. The run uses no randomness.
"""
import argparse
import collections
import concurrent.futures
import hashlib
import http.client
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from check_kill_loop import APPROVAL_TOKEN, UPGRADE_TOKEN, KillableQueue
from check_task_queue import FOLDER, TOKEN

ENV = {**os.environ, 'QUEUE_TOKEN': TOKEN, 'QUEUE_UPGRADE': UPGRADE_TOKEN,
       'QUEUE_APPROVAL': APPROVAL_TOKEN}
SETTINGS = ('intent', 'everything')
WORKING = ('learnability/phase6/strength/working',
           'learnability/phase6/strength/results/4-after.json')
SHORT = 2
PROGRAM = ('main.mo', 'job-queue.mo')


def sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


# A wrong version replaces named functions of the measured program, as `mo test --strength`
# applies it: a function keeps its signature, promises and examples, and takes the body.

def header_end(lines, at):
    depth = 0
    while True:
        depth += sum(lines[at].count(c) for c in '([{') - sum(lines[at].count(c) for c in ')]}')
        at += 1
        if depth == 0:
            break
    while at < len(lines) and (lines[at] == '' or lines[at].startswith(('  promise ',
                                                                         '  examples'))):
        at = at + 1 if lines[at] == '' else lines.index('  end', at) + 1
    return at


def functions(lines):
    """Each top-level function's name and the index of its first and last line."""
    return [(line[3:].split('(')[0], at, lines.index('end', at))
            for at, line in enumerate(lines) if line.startswith('fn ')]


def apply_wrong(wrong, program, out):
    lines = wrong.read_text().splitlines()
    replaced = functions(lines)
    inside = {at for _, start, end in replaced for at in range(start, end + 1)}
    assert all(at in inside or not line or line.startswith(('##', 'module '))
               for at, line in enumerate(lines)), f'{wrong.name} replaces more than functions'
    module = lines[0].removeprefix('module ')
    name = next(n for n in PROGRAM if f'module {module}' in (program / n).read_text().splitlines())
    own = (program / name).read_text().splitlines()
    shutil.copytree(program, out)
    for function, start, end in replaced:
        targets = [(s, e) for n, s, e in functions(own) if n == function]
        assert len(targets) == 1, f'{wrong.name}: {function} is missing or overloaded'
        s, e = targets[0]
        own[header_end(own, s):e + 1] = lines[header_end(lines, start):end + 1]
    (out / name).write_text('\n'.join(own) + '\n')


class Case:
    def __init__(self, name, kind, folder, killed=None):
        self.name, self.kind, self.folder, self.killed = name, kind, folder, killed

    def label(self):
        if self.kind != 'wrong':
            return self.kind
        return 'wrong killed' if self.killed else 'wrong survived'

    def expected(self, setting):
        if self.kind == 'wrong' and self.killed:
            return ('refused',)
        settled = 'held' if self.kind == 'intent' or setting == 'everything' else 'activated'
        return ('refused', settled) if self.kind == 'wrong' else (settled,)

    def hold_worthy(self):
        return self.kind == 'intent'


class Proof:
    def __init__(self, binary, root, jobs):
        self.binary, self.root, self.jobs = binary, root, jobs
        self.cases = []
        self.problems = []
        self.lock = threading.Lock()

    def problem(self, text, tag=None):
        with self.lock:
            self.problems.append((tag, text))

    # The cases.

    def planted(self, folder):
        manifest = json.loads((folder / 'manifest.json').read_text())
        for name in PROGRAM:
            if manifest['program'][name] != sha256(pathlib.Path(FOLDER) / name):
                self.problem(f'planted set {manifest["set"]}: {FOLDER}/{name} is not the program '
                             'its manifest names')
        cases = []
        for change in manifest['changes']:
            out = self.root / 'cases' / change['id']
            shutil.copytree(FOLDER, out)
            for name, digest in change['files'].items():
                if sha256(folder / change['id'] / name) != digest:
                    self.problem(f'{change["id"]}/{name} is not the file its manifest hashes')
                shutil.copyfile(folder / change['id'] / name, out / name)
            kind = change.get('corrected_kind', change['kind'])
            assert kind in ('fix', 'intent'), change
            cases.append(Case(change['id'], kind, out))
        return cases

    def wrong(self, folder, record):
        counted = set((folder / 'counted').read_text().split())
        cases = []
        for version in json.loads(record.read_text())['versions']:
            if version['name'] not in counted or \
                    version['outcome'] in ('invalid', 'changes_intent'):
                continue
            path = folder / version['name']
            if sha256(path) != version['sha256']:
                self.problem(f'{path} is not the version its strength record hashes')
            out = self.root / 'cases' / path.stem
            apply_wrong(path, pathlib.Path(FOLDER), out)
            cases.append(Case(path.stem, 'wrong', out, version['outcome'].startswith('killed')))
        return cases

    # Processes.

    def review(self, queue, *args):
        return subprocess.run(
            [str(self.binary), 'review', '--url', f'http://127.0.0.1:{queue.port}/mcp',
             '--token-env', 'QUEUE_TOKEN', '--approval-token-env', 'QUEUE_APPROVAL', *args],
            capture_output=True, timeout=600, check=False, env=ENV)

    def items(self, queue):
        result = self.review(queue, '--items')
        assert result.returncode == 0, result
        lines = result.stdout.decode().splitlines()
        program = lines[0].removeprefix('program ')
        marks = [line.split()[:2] for line in lines[2:]
                 if line.startswith(('approved ', 'unapproved '))]
        return program, lines[1], marks

    def status(self, queue):
        connection = http.client.HTTPConnection('127.0.0.1', queue.port, timeout=60)
        connection.request('POST', '/mcp', json.dumps({
            'jsonrpc': '2.0', 'id': 1, 'method': 'tools/call',
            'params': {'name': 'mo-upgrade-status', 'arguments': {}}}), {
            'Authorization': f'Bearer {TOKEN}', 'Content-Type': 'application/json',
            'MCP-Protocol-Version': '2025-11-25',
            'Mo-Upgrade-Authorization': f'Bearer {UPGRADE_TOKEN}'})
        body = json.loads(connection.getresponse().read())
        connection.close()
        assert not body['result']['isError'], body
        return body['result']['structuredContent']

    def snapshot(self):
        """Folders holding the served task queue with every item approved, one per setting."""
        folders = {}
        data = self.root / 'approved-everything'
        data.mkdir()
        queue = KillableQueue(self.binary, data, pathlib.Path(FOLDER) / 'main.mo')
        try:
            program, setting, marks = self.items(queue)
            assert setting.startswith('review setting: everything;') and marks, (setting, marks)
            result = self.review(queue, '--base', program,
                                 *(arg for _, digest in marks for arg in ('--approve', digest)))
            assert result.returncode == 0, result
            program, setting, marks = self.items(queue)
            assert {mark for mark, _ in marks} == {'approved'}, marks
        finally:
            queue.stop()
        folders['everything'] = data
        folders['intent'] = self.root / 'approved-intent'
        shutil.copytree(data, folders['intent'])
        queue = KillableQueue(self.binary, folders['intent'], None)
        try:
            assert self.review(queue, '--setting', 'intent').returncode == 0
        finally:
            queue.stop()
        self.base = program
        self.approved = len(marks)
        return folders

    def candidate(self, case):
        result = subprocess.run(
            [str(self.binary), 'check', str(case.folder / 'main.mo'), '--upgrade-from',
             f'{FOLDER}/main.mo', '--json'], capture_output=True, timeout=120, check=False)
        report = json.loads(result.stdout)
        assert result.returncode == 0 and report['base_source'] == self.base, report
        return report['candidate_source']

    # One push.

    def push(self, case, setting, folder, answer):
        """The outcome of pushing `case`, and the door's classification when it decided one."""
        data = self.root / 'runs' / f'{case.name}-{setting}'
        shutil.copytree(folder, data)
        candidate = self.candidate(case)
        queue = KillableQueue(self.binary, data, None)
        try:
            return self.push_to(queue, case, setting, candidate, answer)
        finally:
            queue.stop()

    def push_to(self, queue, case, setting, candidate, answer):
        where = f'{case.name} under {setting}'
        stop, unanswered = threading.Event(), []

        def calls():
            while not stop.is_set():
                time.sleep(0.1)
                try:
                    status, body = queue.rpc_post('stats', {})
                    if status != 200 or json.loads(body)['result']['isError']:
                        unanswered.append(f'HTTP {status}: {body[:200]!r}')
                except (OSError, http.client.HTTPException) as error:
                    unanswered.append(repr(error))

        caller = threading.Thread(target=calls)
        caller.start()
        try:
            result = subprocess.run(
                [str(self.binary), 'push', str(case.folder / 'main.mo'), '--url',
                 f'http://127.0.0.1:{queue.port}/mcp', '--base', self.base, '--token-env',
                 'QUEUE_TOKEN', '--upgrade-token-env', 'QUEUE_UPGRADE', '--json'],
                capture_output=True, timeout=900, check=False, env=ENV)
        finally:
            stop.set()
            caller.join()
        if unanswered:
            self.problem(f'{where}: the old program left {len(unanswered)} calls unanswered '
                         f'during the push, first {unanswered[0]}')
        reports = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        report = reports[-1] if reports else {}
        outcome = {0: 'activated', 3: 'held'}.get(result.returncode, 'refused')
        if outcome == 'refused' and report.get('code') is None:
            outcome = f'failed (exit {result.returncode})'
        code = report.get('code')
        classification = None
        if outcome in ('activated', 'held'):
            latest = self.status(queue)['decisions'][0]
            if latest['kind'] != outcome or latest['candidate_source'] != candidate:
                self.problem(f'{where}: the push {outcome}, the latest decision row is {latest}')
            classification = not latest['intent_would_activate']
        active, _, _ = self.items(queue)
        if active != (candidate if outcome == 'activated' else self.base):
            self.problem(f'{where}: after the push {outcome}, the active program is {active}')
        if outcome == 'held' and answer is not None:
            self.answer(queue, where, answer, report['review'], candidate)
        return outcome, code, classification, result

    def answer(self, queue, where, answer, review, candidate):
        result = self.review(queue, f'--{answer}', review)
        active, _, _ = self.items(queue)
        latest = self.status(queue)['decisions'][0]
        if result.returncode != 0 or latest['kind'] != answer or \
                active != (candidate if answer == 'yes' else self.base):
            self.problem(f'{where}: --{answer} exited {result.returncode} '
                         f'{result.stderr.decode()[-300:]!r}; the active program is {active} and '
                         f'the latest decision {latest["kind"]}')
            return
        with self.lock:
            self.answered[answer] += 1

    # The run.

    def go(self, folders):
        self.answered = collections.Counter()
        plan = [(case, setting) for case in self.cases for setting in SETTINGS]
        answers = iter(('yes', 'no'))
        chosen = {pair: next(answers, None) for pair in plan
                  if pair[0].expected(pair[1]) == ('held',)}
        counts = collections.Counter()
        self.outcomes = {}
        with concurrent.futures.ThreadPoolExecutor(self.jobs) as pool:
            futures = {pair: pool.submit(self.push, *pair, folders[pair[1]], chosen.get(pair))
                       for pair in plan}
            for (case, setting), future in futures.items():
                outcome, code, classification, result = future.result()
                counts[(case.label(), setting, outcome)] += 1
                self.outcomes[(case.name, setting)] = outcome
                expected = case.expected(setting)
                if outcome not in expected or (outcome == 'refused' and code != 'E1433'):
                    self.problem(f'{case.name} ({case.label()}) under {setting}: expected '
                                 f'{" or ".join(expected)}, refusals E1433, got '
                                 f'{outcome} {code or ""}: {result.stdout[-400:]!r} '
                                 f'{result.stderr[-400:]!r}', (case.name, setting))
                if classification is not None and classification != case.hold_worthy():
                    worthy = 'hold-worthy' if classification else 'not hold-worthy'
                    self.problem(f'{case.name} ({case.label()}) under {setting}: the door '
                                 f'classifies it {worthy}')
        for answer in ('yes', 'no'):
            if self.answered[answer] == 0:
                self.problem(f'no held push was answered --{answer}', 'answer')
        return counts


def run(binary, options, root, label):
    proof = Proof(binary, root, options.jobs)
    planted = proof.planted(options.planted)
    wrong = [case for folder, record in options.wrong for case in proof.wrong(folder, record)]
    if not options.all:
        planted = [case for kind in ('fix', 'intent')
                   for case in [c for c in planted if c.kind == kind][:SHORT]]
        wrong = [c for c in wrong if c.killed][:SHORT] + [c for c in wrong if not c.killed][:1]
    proof.cases = planted + wrong
    for label_ in ('fix', 'intent', 'wrong killed'):
        if not any(case.label() == label_ for case in proof.cases):
            proof.problem(f'no case of kind {label_}')
    folders = proof.snapshot()
    began = time.monotonic()
    counts = proof.go(folders)
    print(f'door proof ({label}): seed=none cases={len(proof.cases)} approved={proof.approved} '
          f'pushes={sum(counts.values())} yes={proof.answered["yes"]} no={proof.answered["no"]} '
          f'seconds={time.monotonic() - began:.0f}')
    for (kind, setting, outcome), n in sorted(counts.items()):
        print(f'  {kind} under {setting}: {n} {outcome}')
    return proof


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('mo')
    parser.add_argument('planted', type=pathlib.Path)
    parser.add_argument('--bypass', help='a mo built with the push decision bypassed')
    parser.add_argument('--all', action='store_true', help='push every case, not the short run')
    parser.add_argument('--wrong', nargs=2, action='append', type=pathlib.Path,
                        metavar=('SET', 'RECORD'), help='a frozen wrong-version set and the '
                        'strength record that measured it; the working half by default')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--keep', action='store_true', help='keep the data folders after a failure')
    options = parser.parse_args()
    options.wrong = options.wrong or [tuple(map(pathlib.Path, WORKING))]
    if not (options.planted / 'manifest.json').is_file():
        print(f'door proof: no planted changes at {options.planted}', file=sys.stderr)
        sys.exit(1)
    if options.bypass and not os.access(options.bypass, os.X_OK):
        print(f'door proof: no bypassed build at {options.bypass}', file=sys.stderr)
        sys.exit(1)
    root = pathlib.Path(tempfile.mkdtemp(prefix='mo-door-'))
    failed = True
    try:
        proof = run(pathlib.Path(options.mo).resolve(), options, root / 'door', 'door')
        for _, text in proof.problems:
            print(f'mismatch: {text}', file=sys.stderr)
        failed = bool(proof.problems)
        if options.bypass:
            control = run(pathlib.Path(options.bypass).resolve(), options, root / 'bypass',
                          'decision bypassed')
            held = sorted(pair for pair, outcome in proof.outcomes.items() if outcome == 'held')
            outcomes = sorted(tag for tag, _ in control.problems if isinstance(tag, tuple))
            others = [text for tag, text in control.problems if tag is None]
            if outcomes != held or others:
                failed = True
                print(f'mismatch: the bypassed build mismatched {outcomes}; the control '
                      f'requires exactly {held}', file=sys.stderr)
                for text in others:
                    print(f'mismatch: under the bypass, {text}', file=sys.stderr)
            else:
                print(f'door proof: the bypassed build fails with the {len(held)} expected '
                      'mismatches')
    finally:
        if failed and options.keep:
            print(f'door proof: data kept at {root}', file=sys.stderr)
        else:
            shutil.rmtree(root, ignore_errors=True)
    print(f'door proof: result={"fail" if failed else "pass"}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
