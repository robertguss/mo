#!/usr/bin/env python3
"""Kill the task queue under a real mo serve at random moments and check what its client saw.

The driver sends a seeded sequence of tool calls over loopback, SIGKILLs the server a random
delay after a request is sent, restarts it on the same data folder, and reads every job. It
then reconciles the jobs with a model built only from replies:

- every update whose reply arrived is in the queue;
- the one update sent without a reply applied in full or not at all;
- nothing is in the queue that was never sent.

A reply's runtime error says what the call did through its `effect`: `none` is not applied
(rate_limited, busy and invalid_arguments among them), `applied` is applied, and `unknown`
counts like a lost reply. A connection that is refused, reset or closed before the reply is a
lost reply.

Usage: check_kill_loop.py MO [--seed N] [--kills N] [--keep]. With no options it makes the
short run in the drivers group. Timing decides which kills land mid-commit, so the short run
only prints that count; a run with more kills than the short one fails if none did.
"""
import argparse
import http.client
import json
import pathlib
import random
import shutil
import statistics
import sys
import tempfile
import threading
import time

from check_task_queue import ALL_JOBS, TOKEN, Queue


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
    """A Queue whose post says when the request has gone, so a kill can follow it."""

    def __init__(self, binary, data):
        super().__init__(binary, data)
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
        self.start()

    def start(self):
        # The constructor returns only after the listening line: a SIGKILL before a folder's
        # first one can leave a zero-byte service.sqlite that the store then refuses for good.
        self.queue = KillableQueue(self.binary, self.folder)
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
        self.new_folder()
        while self.killed < self.kills:
            if len(self.model) > FOLDER_JOBS or time.monotonic() - self.born > FOLDER_SECONDS:
                self.new_folder()
            for _ in range(self.random.randrange(2, 10)):
                if self.random.random() < 0.03:
                    self.burst()
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
          f'folders={run.folders} calls={run.calls} answered={run.answered} '
          f'refused={run.refused} unknown={run.unknown} '
          f'result={"fail" if failure else "pass"} seconds={time.monotonic() - began:.0f}')
    if failure is not None:
        print(f'kill loop failed: seed {run.seed}, kill {run.killed}, call {run.calls}:\n{failure}',
              file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
