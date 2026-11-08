#!/usr/bin/env python3
"""Kill the task queue under a real mo serve at random moments and check what its client saw.

The driver sends a seeded sequence of tool calls over loopback, SIGKILLs the server a random
delay after a request is sent, restarts it on the same data folder, and reads every job. It
then reconciles the jobs with a model built only from replies:

- every update whose reply arrived is in the queue;
- the one update sent without a reply applied in full or not at all;
- nothing is in the queue that was never sent.

Between each kill and the restart it runs `mo history` on the folder, which must either read it
or report that recovery is required: a kill leaves no folder the reader cannot classify.

A reply's runtime error says what the call did through its `effect`: `none` is not applied
(rate_limited, busy and invalid_arguments among them), `applied` is applied, and `unknown`
counts like a lost reply. A connection that is refused, reset or closed before the reply is a
lost reply.

Each folder also receives up to two live upgrades with `mo push` to tool-only variants, and
pushes of an invalid variant that changes the access clause. Callers keep sending while a push
runs, and some pushes are killed at a random moment. After every kill the folder restarts from
its recorded program alone; the active program must be the one before or after the push in
flight, the upgrade records must count one per accepted activation, and an invalid push must
never activate.

Usage: check_kill_loop.py MO [--seed N] [--kills N] [--keep]. With no options it makes the
short run in the drivers group. Timing decides which kills land mid-commit, so the short run
only prints that count; a run with more kills than the short one fails if none did.
"""
import argparse
import http.client
import json
import os
import pathlib
import random
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time

from check_task_queue import ALL_JOBS, FOLDER, TOKEN, Queue

UPGRADE_TOKEN = 'KillLoopUpgradeCredential0123'


SHORT_SEED = 1
SHORT_KILLS = 15
PACE = 0.06  # seconds before each call, under the token's 20 calls per second
MAX_ATTEMPTS = 3  # the third failed attempt makes a job Dead
# A data folder is replaced before the queue's 5,000-job limit, and long before a 30-minute
# lease could expire, so the model never has to expire a lease.
FOLDER_JOBS = 500
FOLDER_SECONDS = 20 * 60
# SQLite keeps the journal file in exclusive locking mode and zeroes its header at each
# commit, so only a journal that still starts with this magic was left by a commit in progress.
JOURNAL_MAGIC = bytes.fromhex('d9d505f920a163d7')
LOST = (OSError, http.client.HTTPException)


class Mismatch(Exception):
    pass


class KillableQueue(Queue):
    """A Queue whose post says when the request has gone, so a kill can follow it. It accepts
    upgrades, and after its folder's first start it runs the recorded program alone."""

    def __init__(self, binary, data, source):
        self.process = subprocess.Popen(
            [str(binary), 'serve', *([str(source)] if source else []), '--port', '0', '--data',
             str(data), '--upgrade-token-env', 'QUEUE_UPGRADE'],
            env={**os.environ, 'QUEUE_TOKEN': TOKEN, 'QUEUE_UPGRADE': UPGRADE_TOKEN},
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        try:
            self.port = self.listening()
        except BaseException:
            self.kill()
            raise
        self.next_id = 100
        self.sent = threading.Event()

    def post(self, body, token=TOKEN):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=30)
        try:
            connection.request('POST', '/mcp', body, {
                'Authorization': f'Bearer {token}', 'Content-Type': 'application/json',
                'MCP-Protocol-Version': '2025-11-25'})
            self.sent.set()
            response = connection.getresponse()
            return response.status, response.read()
        finally:
            self.sent.set()
            connection.close()

    def rpc_post(self, tool, arguments):
        self.next_id += 1
        return self.post(json.dumps({'jsonrpc': '2.0', 'id': self.next_id, 'method': 'tools/call',
                                     'params': {'name': tool, 'arguments': arguments}}))


# The model: id -> (payload, attempts, state), where state is Ready, Leased, Done or Dead.
# Lease times are not modelled: no lease expires within a folder's life.

def project(jobs):
    return {int(job['id']): (job['payload'], int(job['attempts']), job['state']['case'])
            for job in jobs}


def oldest_ready(model):
    return min((id for id, job in model.items() if job[2] == 'Ready'), default=None)


def holds(model, id, attempt):
    return id in model and model[id][2] == 'Leased' and model[id][1] == attempt


def predict(model, call):
    """The model after `call` succeeds, and the reply it should get."""
    tool, arguments = call
    after = dict(model)
    if tool == 'submit':
        id = len(model) + 1
        after[id] = (arguments['payload'], 0, 'Ready')
        return after, {'value': str(id)}
    if tool == 'lease':
        id = oldest_ready(model)
        if id is None:
            return after, {'value': {'case': 'None'}}
        payload, attempts, _ = model[id]
        after[id] = (payload, attempts + 1, 'Leased')
        return after, {'value': {'case': 'Some', 'value': {
            'id': str(id), 'payload': payload, 'attempts': str(attempts + 1)}}}
    id, attempt = int(arguments['id']), int(arguments['attempt'])
    if id not in model:
        return after, {'error': {'kind': 'application', 'value': {
            'case': 'NoSuchJob', 'fields': {'id': str(id)}}}}
    if not holds(model, id, attempt):
        return after, {'error': {'kind': 'application', 'value': {
            'case': 'NotLeased', 'fields': {'id': str(id)}}}}
    payload, attempts, _ = model[id]
    if tool == 'acknowledge':
        after[id] = (payload, attempts, 'Done')
    elif tool == 'fail':
        after[id] = (payload, attempts, 'Dead' if attempts >= MAX_ATTEMPTS else 'Ready')
    return after, {'value': None}


def matches(content, expected):
    """A reply equals its prediction; a leased job's `until` is not predicted."""
    if 'value' in content and isinstance(content['value'], dict) and \
            content['value'].get('case') == 'Some':
        job = dict(content['value']['value'])
        state = job.pop('state')
        return state['case'] == 'Leased' and \
            {'value': {'case': 'Some', 'value': job}} == expected
    return content == expected


def difference(model, observed):
    lines = []
    for id in sorted(set(model) | set(observed)):
        if model.get(id) != observed.get(id):
            lines.append(f'  job {id}: model {model.get(id)}, queue {observed.get(id)}')
    return '\n'.join(lines)


class Run:
    def __init__(self, binary, seed, kills, root):
        self.binary, self.seed, self.kills, self.root = binary, seed, kills, root
        self.random = random.Random(seed)
        self.queue = None
        self.folder = None
        self.started = False
        self.born = 0.0
        self.folders = 0
        self.model = {}
        self.payloads = 0
        self.killed = 0
        self.mid_commit = 0
        self.calls = 0
        self.answered = 0
        self.refused = 0
        self.unknown = 0
        self.latencies = [0.03]
        self.history_read = 0
        self.history_recovery = 0
        self.upgrades = []  # the source folder of each program: the original, then variants
        self.program = 0  # index in self.upgrades of the active program
        self.ordinal = 0  # activations the folder has committed
        self.recorded = None  # upgrade records the last mo history read
        self.pushes = self.activated = self.invalid = self.push_kills = 0

    def fail(self, message):
        raise Mismatch(message)

    # Data folders and processes.

    def new_folder(self):
        if self.queue is not None:
            self.queue.stop()
            self.queue = None
            shutil.rmtree(self.folder)
        self.folders += 1
        self.folder = self.root / f'folder-{self.folders}'
        self.folder.mkdir()
        self.started = False
        self.model = {}
        self.program = self.ordinal = 0
        self.recorded = None
        self.start()

    def start(self):
        # The constructor returns only after the listening line: a SIGKILL before a folder's
        # first one can leave a zero-byte service.sqlite that the store then refuses for good.
        try:
            self.queue = KillableQueue(self.binary, self.folder,
                                       None if self.started else self.upgrades[0] / 'main.mo')
        except AssertionError as error:
            # A server that does not come back after a kill is a finding, so --keep keeps its
            # folder; the queue has already killed the process it started.
            self.fail(f'mo serve did not start on {self.folder}: {error}')
        if not self.started:
            self.started, self.born = True, time.monotonic()

    def kill(self):
        assert self.started, 'never kill before the first listening line of a folder'
        self.queue.process.kill()
        self.queue.process.wait(timeout=30)
        self.queue.close()
        self.queue = None
        self.killed += 1
        journal = self.folder / 'service.sqlite-journal'
        if journal.exists():
            with journal.open('rb') as file:
                if file.read(len(JOURNAL_MAGIC)) == JOURNAL_MAGIC:
                    self.mid_commit += 1
        self.history()

    def history(self):
        """mo history on the killed folder: a normal read or "recovery required", nothing else."""
        # A folder that needs recovery says nothing about its upgrade records yet.
        self.recorded = None
        result = subprocess.run([str(self.binary), 'history', '--json', str(self.folder)],
                                capture_output=True, timeout=300, check=False)
        lines = result.stdout.decode('utf-8', 'replace').splitlines()
        folder = json.loads(lines[1]).get('folder') if len(lines) > 2 else None
        if (result.returncode, folder) == (0, 'ok'):
            self.history_read += 1
            for line in lines:
                if line.startswith('{"record":"history"') or '"upgrades":' in line:
                    self.recorded = len(json.loads(line).get('upgrades', []))
        elif (result.returncode, folder) == (1, 'recovery_required'):
            self.history_recovery += 1
        else:
            self.fail(f'mo history after a kill exited {result.returncode} with folder {folder}: '
                      f'{result.stdout[-500:]!r} {result.stderr[-500:]!r}')

    # Calls.

    def payload(self):
        self.payloads += 1
        padding = 'x' * self.random.randrange(64)
        return f'kill loop seed {self.seed} payload {self.payloads} {padding}'

    def choose(self):
        """A tool call that makes sense for the model, with a few stale ones."""
        held = [(id, job[1]) for id, job in sorted(self.model.items()) if job[2] == 'Leased']
        roll = self.random.random()
        if roll < 0.35 or not self.model:
            return 'submit', {'payload': self.payload()}
        if roll < 0.55 or not held:
            return 'lease', {}
        if roll < 0.94:
            id, attempt = self.random.choice(held)
            tool = self.random.choice(['heartbeat', 'acknowledge', 'acknowledge', 'fail'])
            return tool, {'id': str(id), 'attempt': str(attempt)}
        id = self.random.choice(sorted(self.model))
        tool = self.random.choice(['heartbeat', 'acknowledge', 'fail'])
        return tool, {'id': str(id), 'attempt': str(self.model[id][1] + 1)}

    def send(self, call, pace=True):
        """The reply's structuredContent, or None when the reply was lost."""
        tool, arguments = call
        self.calls += 1
        if pace:
            time.sleep(PACE)
        began = time.monotonic()
        try:
            status, body = self.queue.rpc_post(tool, arguments)
        except LOST:
            return None
        if status != 200:
            self.fail(f'{tool} answered HTTP {status}: {body!r}')
        result = json.loads(body)['result']
        if not result['isError']:
            self.latencies = (self.latencies + [time.monotonic() - began])[-50:]
        return result['structuredContent']

    def judge(self, call, content):
        """Applies an answered call to the model, or returns the outcomes a lost one allows."""
        after, expected = predict(self.model, call)
        if content is None:
            self.unknown += 1
            return [self.model, after]
        error = content.get('error')
        effect = error['effect'] if error is not None and error['kind'] == 'runtime' else None
        if effect == 'unknown':
            self.unknown += 1
            return [self.model, after]
        self.answered += 1
        if effect == 'none':
            self.refused += 1
            return None
        if effect == 'applied':
            self.model = after
            return None
        if effect is not None:
            self.fail(f'{call[0]} answered an effect outside none, applied, unknown: {content}')
        if not matches(content, expected):
            self.fail(f'{call[0]} {call[1]} answered {content}, the model expected {expected}')
        self.model = after
        return None

    def step(self):
        outcomes = self.judge(call := self.choose(), self.send(call))
        if outcomes is not None:
            self.kill()
            self.restart(call, outcomes)

    def burst(self):
        """Spends the token's rate allowance, then sends real calls until one is refused."""
        for _ in range(200):
            content = self.send(('submit', {}), pace=False)
            if content is None:
                self.fail('a call during the burst lost its reply')
            self.answered += 1
            code = content['error']['code']
            if code == 'rate_limited':
                break
            if code != 'invalid_arguments':
                self.fail(f'submit without a payload answered {content}')
        for _ in range(5):
            refused = self.refused
            outcomes = self.judge(call := self.choose(), self.send(call, pace=False))
            if outcomes is not None:
                self.kill()
                self.restart(call, outcomes)
                return
            if self.refused > refused:
                return

    def kill_during(self):
        """Sends a call and kills the server a random delay after the request has gone."""
        call = self.choose()
        reply = {}
        self.queue.sent.clear()

        def send():
            try:
                reply['content'] = self.send(call)
            except BaseException as error:  # reported by the main thread
                reply['error'] = error

        sender = threading.Thread(target=send)
        sender.start()
        if not self.queue.sent.wait(timeout=30):
            self.fail('the request was never sent')
        time.sleep(self.random.uniform(0, 1.3 * statistics.median(self.latencies)))
        self.kill()
        sender.join(timeout=60)
        if sender.is_alive():
            self.fail('the client never returned after the kill')
        if 'error' in reply:
            raise reply['error']
        outcomes = self.judge(call, reply['content'])
        self.restart(call, outcomes or [self.model])

    # Upgrades.

    def variants(self):
        """The original queue, two tool-only variants and one that changes its access clause."""
        source = (pathlib.Path(FOLDER) / 'main.mo').read_text()
        for name, text in [
                ('v0', source),
                *[(f'v{n}', source.replace(
                    'tools submit, lease, heartbeat, acknowledge, fail, list, stats',
                    'tools submit, lease, heartbeat, acknowledge, fail, list, stats, version') +
                   f'\nfn version(_queue: Queue, _now: Time) : Text\n  "{n}"\nend\n')
                  for n in (1, 2)],
                ('invalid', source.replace('token from env "QUEUE_TOKEN"',
                                           'token from env "QUEUE_OTHER"'))]:
            folder = self.root / name
            shutil.copytree(pathlib.Path(FOLDER), folder)
            (folder / 'main.mo').write_text(text)
            self.upgrades.append(folder)

    def digest(self, index):
        result = subprocess.run([str(self.binary), 'check', str(self.upgrades[index] / 'main.mo'),
                                 '--upgrade-from', str(self.upgrades[0] / 'main.mo'), '--json'],
                                capture_output=True, timeout=120, check=False)
        return json.loads(result.stdout)['candidate_source']

    def active(self):
        """The variant the server runs, read through the version tool the variants add."""
        time.sleep(PACE)
        status, body = self.queue.rpc_post('version', {})
        if status != 200:
            self.fail(f'version answered HTTP {status}: {body!r}')
        reply = json.loads(body)
        if 'error' in reply:
            return 0
        return int(reply['result']['structuredContent']['value'])

    def push(self, target, out):
        out['result'] = subprocess.run(
            [str(self.binary), 'push', str(self.upgrades[target] / 'main.mo'), '--url',
             f'http://127.0.0.1:{self.queue.port}/mcp', '--base', self.digests[self.program],
             '--token-env', 'QUEUE_TOKEN', '--upgrade-token-env', 'QUEUE_UPGRADE', '--json'],
            capture_output=True, timeout=300, check=False,
            env={**os.environ, 'QUEUE_TOKEN': TOKEN, 'QUEUE_UPGRADE': UPGRADE_TOKEN})

    def upgrade(self):
        """Pushes the next variant, or the invalid one, while calls go on; may kill mid-push."""
        target = self.program + 1 if self.program < 2 and self.random.random() < 0.8 else 3
        killing = self.random.random() < 0.5
        self.pushes += 1
        out = {}
        pusher = threading.Thread(target=self.push, args=(target, out))
        pusher.start()
        deadline = time.monotonic() + self.random.uniform(0.05, 1.5)
        call, outcomes = None, None
        while pusher.is_alive() and (not killing or time.monotonic() < deadline):
            outcomes = self.judge(call := self.choose(), self.send(call))
            if outcomes is not None:
                self.fail(f'{call[0]} lost its reply during a push with no kill')
        if killing and pusher.is_alive():
            self.push_kills += 1
            self.kill()
            pusher.join(timeout=300)
            self.restart(call or ('stats', {}), [self.model])
            now = self.active()
            allowed = {self.program} | ({target} if target != 3 else set())
            if now not in allowed:
                self.fail(f'after a kill during a push to variant {target}, the server runs '
                          f'variant {now}; it ran {self.program}')
            if now != self.program:
                self.program, self.ordinal = now, self.ordinal + 1
                self.activated += 1
            if self.recorded is not None and self.recorded != self.ordinal:
                self.fail(f'the folder records {self.recorded} upgrades; {self.ordinal} were '
                          'committed')
            return
        pusher.join(timeout=300)
        result = out['result']
        report = json.loads(result.stdout) if result.stdout else {}
        if target == 3:
            self.invalid += 1
            if result.returncode != 1 or report.get('code') != 'E1426':
                self.fail(f'the invalid variant was not refused for its access clause: {result}')
        elif result.returncode != 0 or report.get('ordinal') != str(self.ordinal + 1):
            self.fail(f'push to variant {target} failed: {result}')
        else:
            self.program, self.ordinal = target, self.ordinal + 1
            self.activated += 1
        if self.active() != self.program:
            self.fail(f'the server does not run variant {self.program} after its push')

    # Reconciling.

    def listing(self, case):
        """Every job matching `case`, read as the list tool tells agents to."""
        jobs, after, limit = [], '0', 100
        while True:
            time.sleep(PACE)
            status, body = self.queue.rpc_post(
                'list', {'filter': {'case': case}, 'after': after, 'limit': str(limit)})
            if status != 200:
                self.fail(f'list answered HTTP {status}: {body!r}')
            result = json.loads(body)['result']
            content = result['structuredContent']
            if result['isError']:
                if content['error'].get('code') != 'response_limit' or limit == 1:
                    self.fail(f'list answered {content}')
                limit //= 2
                continue
            jobs += content['value']['jobs']
            if content['value']['next']['case'] == 'None':
                return jobs
            after = content['value']['next']['value']

    def restart(self, call, outcomes):
        """Restarts on the same folder and requires the queue to be one of `outcomes`."""
        self.start()
        if call[0] == 'lease' and len(outcomes) == 2:
            leased = set(project(self.listing('Leased')))
            known = {id for id, job in self.model.items() if job[2] == 'Leased'}
            if not known <= leased or len(leased - known) > 1 or \
                    not leased - known <= {oldest_ready(self.model)}:
                self.fail(f'after a lease without a reply, the queue leases {sorted(leased)}; '
                          f'the model knows {sorted(known)} and allows only job '
                          f'{oldest_ready(self.model)} more')
        observed = project(self.listing(ALL_JOBS['case']))
        for outcome in outcomes:
            if observed == outcome:
                self.model = outcome
                return
        lines = [f'after {call[0]} {call[1]}, the recovered queue is none of the '
                 f'{len(outcomes)} allowed outcomes']
        for index, outcome in enumerate(outcomes):
            lines.append(f' outcome {index + 1}:\n{difference(outcome, observed)}')
        self.fail('\n'.join(lines))

    # The loop.

    def go(self):
        self.variants()
        self.digests = [self.digest(index) for index in range(3)]
        self.new_folder()
        while self.killed < self.kills:
            # A folder that has both upgrades is replaced now and then, so pushes keep coming.
            if len(self.model) > FOLDER_JOBS or time.monotonic() - self.born > FOLDER_SECONDS or \
                    (self.program == 2 and self.random.random() < 0.05):
                self.new_folder()
            for _ in range(self.random.randrange(2, 10)):
                roll = self.random.random()
                if roll < 0.03:
                    self.burst()
                elif roll < 0.06 + (0.1 if self.program < 2 else 0):
                    self.upgrade()
                else:
                    self.step()
            self.kill_during()
        self.queue.stop()
        self.queue = None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('mo')
    parser.add_argument('--seed', type=int, default=SHORT_SEED)
    parser.add_argument('--kills', type=int, default=SHORT_KILLS)
    parser.add_argument('--keep', action='store_true',
                        help='keep the data folder after a failure')
    options = parser.parse_args()
    binary = pathlib.Path(options.mo).resolve()
    root = pathlib.Path(tempfile.mkdtemp(prefix='mo-kill-loop-'))
    run = Run(binary, options.seed, options.kills, root)
    began = time.monotonic()
    failure = None
    try:
        run.go()
        if run.kills > SHORT_KILLS and run.mid_commit == 0:
            failure = 'no kill landed mid-commit'
    except Mismatch as error:
        failure = str(error)
    except BaseException:
        print(f'kill loop: seed {run.seed}, kill {run.killed + 1}, call {run.calls}: '
              f'the driver stopped', file=sys.stderr)
        raise
    finally:
        if run.queue is not None:
            run.queue.kill()
        if failure is not None and options.keep:
            print(f'kill loop: data folder kept at {run.folder}', file=sys.stderr)
        else:
            shutil.rmtree(root, ignore_errors=True)
    print(f'kill loop: seed={run.seed} kills={run.killed} mid_commit={run.mid_commit} '
          f'folders={run.folders} history_read={run.history_read} '
          f'history_recovery={run.history_recovery} calls={run.calls} answered={run.answered} '
          f'refused={run.refused} unknown={run.unknown} pushes={run.pushes} '
          f'activated={run.activated} invalid={run.invalid} push_kills={run.push_kills} '
          f'result={"fail" if failure else "pass"} seconds={time.monotonic() - began:.0f}')
    if failure is not None:
        print(f'kill loop failed: seed {run.seed}, kill {run.killed}, call {run.calls}:\n{failure}',
              file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
