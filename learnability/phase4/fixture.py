#!/usr/bin/env python3
"""Build, render and check a Phase 4 scenario's fixture: one step list, two consumers.

  python3 learnability/phase4/fixture.py build VARIANT MO OUT TOKEN_ENV
        replay VARIANT/fixture.json into `mo serve VARIANT/main.mo` on an empty data folder,
        in real time at the token's rate limit, then check the result and stop the service.
        Writes OUT/data (the built data folder), OUT/build.json (what was sent, and when) and,
        for a question, OUT/key.json. TOKEN_ENV names the environment variable that holds the
        token; the service reads it as QUEUE_TOKEN.
  python3 learnability/phase4/fixture.py render VARIANT
        write VARIANT/fixture-steps.mo, module FixtureSteps, whose `replay(sim)` sends the same
        steps into a Simulation, each wait becoming Simulation.advance
  python3 learnability/phase4/fixture.py rendered VARIANT
        exit 0 when VARIANT/fixture-steps.mo is what render would write, else name the file
  python3 learnability/phase4/fixture.py check VARIANT URL TOKEN_ENV [STDERR]
        the pre-session check against a running service at URL (http://127.0.0.1:PORT/mcp),
        called directly with the one token it accepts: every assertion in the fixture's
        "check" list, reading STDERR, the service's standard error file, for assertions on it
  python3 learnability/phase4/fixture.py key VARIANT MO DATA TOKEN_ENV
        recompute a question's answer key from a built data folder (served from a copy),
        printing the key.json that build wrote

VARIANT is a folder holding main.mo, job-queue.mo and fixture.json. Exit status 0 means done or
passed, 1 a failed assertion (named on standard error), 2 a bad command line or fixture.

fixture.json is one object:
  "steps": the pre-state, in order. Each step is an object with one of these keys:
    {"submit": TEXT}, optionally with "pad": BYTES, padding the payload with spaces to BYTES
        bytes; "{n}" in TEXT is the enclosing repeat's counter
    {"lease": true}                       lease the oldest ready job
    {"acknowledge": "lease"}, {"fail": "lease"}, {"heartbeat": "lease"}
                                          act on the most recent lease, with its attempt
    {"wait": SECONDS}                     sleep live; Simulation.advance when rendered
    {"repeat": N, "from": K, "steps": [...]}
                                          run the steps N times with n = K, K + 1, ... (K
                                          defaults to 1); repeats do not nest
    {"expect": ASSERTION}                 live only: an assertion the build checks at this point
  "check": assertions the pre-session check makes, and the build makes after its last step
  "key": for a question, the rule the answer is computed by:
    {"rule": "shared_payload_jobs"}       how many jobs have a payload another job also has
    {"rule": "killing_call", "job": ID, "margin": ROWS}
                                          the tool and arrival time, to the second, of the call
                                          that made job ID Dead, read from the log records after
                                          the last snapshot, which must leave ROWS rows free
  "failing_tool": the tool the check calls to see the defect fire, when it fails outright

An ASSERTION is one of
  {"job": ID, "state": "Ready" | "Leased" | "Done" | "Dead", "attempts": N}
  {"call": TOOL, "arguments": {...}, "error": CODE}   the call fails with runtime error CODE
  {"stderr": "report"}           the service's standard error holds a failure line and a report
  {"stderr": "failure_only"}     it holds a failure line and no report after it
"""

import http.client
import json
import os
import pathlib
import select
import shutil
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse

STEP_KINDS = ('submit', 'lease', 'acknowledge', 'fail', 'heartbeat', 'wait', 'repeat', 'expect')
SETTLE = ('acknowledge', 'fail', 'heartbeat')
STATES = ('Ready', 'Leased', 'Done', 'Dead')
TOOLS = ['submit', 'lease', 'heartbeat', 'acknowledge', 'fail', 'list', 'stats']
PROTOCOL = '2025-11-25'
PACE = 0.052  # seconds between calls: under the token's limit of 20 calls a second
# A log record: magic and version, kind, sequence, image key, payload length, then the payload.
PAYLOAD_AT = 5 + 1 + 8 + 8 + 4


class FixtureError(Exception):
    """fixture.json is malformed."""


class CheckFailed(Exception):
    """An assertion about the queue did not hold."""


def load(variant):
    path = pathlib.Path(variant) / 'fixture.json'
    try:
        fixture = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        raise FixtureError(f'{path}: {error}') from error
    if not isinstance(fixture, dict) or not isinstance(fixture.get('steps'), list):
        raise FixtureError(f'{path}: needs an object with a "steps" list')
    unknown = set(fixture) - {'steps', 'check', 'key', 'failing_tool'}
    if unknown:
        raise FixtureError(f'{path}: unknown field {sorted(unknown)[0]!r}')
    validate_steps(fixture['steps'], nested=False)
    for assertion in fixture.get('check', []):
        validate_assertion(assertion)
    key = fixture.get('key')
    if key is not None and key.get('rule') not in ('shared_payload_jobs', 'killing_call'):
        raise FixtureError(f'unknown key rule {key.get("rule")!r}')
    if fixture.get('failing_tool') not in (None, *TOOLS):
        raise FixtureError(f'unknown failing tool {fixture["failing_tool"]!r}')
    return fixture


def step_kind(step):
    kinds = [k for k in STEP_KINDS if k in step] if isinstance(step, dict) else []
    if len(kinds) != 1:
        raise FixtureError(f'a step needs exactly one of {", ".join(STEP_KINDS)}: {step!r}')
    return kinds[0]


def validate_steps(steps, nested, leased=False):
    """leased: whether a lease the rendered module can name precedes these steps. It names a
    lease made in a repeat only inside that repeat's body, so a settle there must follow the
    body's own lease, or the body must make none and act on the one before it."""
    for step in steps:
        kind = step_kind(step)
        allowed = {kind, 'pad'} if kind == 'submit' else {kind, 'from', 'steps'} \
            if kind == 'repeat' else {kind}
        if set(step) - allowed:
            raise FixtureError(f'unknown field in {kind} step: {step!r}')
        if kind == 'submit' and (not isinstance(step['submit'], str) or
                                 not isinstance(step.get('pad', 0), int)):
            raise FixtureError(f'submit takes a text and an optional byte count: {step!r}')
        if kind in SETTLE and step[kind] != 'lease':
            raise FixtureError(f'{kind} acts on "lease", the most recent lease: {step!r}')
        if kind in SETTLE and not leased:
            raise FixtureError(f'{kind} has no lease it can act on here: {step!r}')
        if kind == 'lease':
            leased = True
        if kind == 'wait' and (not isinstance(step['wait'], int) or step['wait'] <= 0):
            raise FixtureError(f'wait takes a positive whole number of seconds: {step!r}')
        if kind == 'expect':
            validate_assertion(step['expect'])
        if kind == 'repeat':
            if nested:
                raise FixtureError('repeats do not nest')
            if not isinstance(step['repeat'], int) or step['repeat'] < 1 or \
                    not isinstance(step.get('from', 1), int):
                raise FixtureError(f'repeat takes a positive count: {step!r}')
            body = step.get('steps', [])
            leases = any(isinstance(s, dict) and 'lease' in s for s in body)
            validate_steps(body, nested=True, leased=leased and not leases)
            leased = leased and not leases


def validate_assertion(assertion):
    if not isinstance(assertion, dict):
        raise FixtureError(f'an assertion is an object: {assertion!r}')
    if 'job' in assertion:
        if set(assertion) != {'job', 'state', 'attempts'} or assertion['state'] not in STATES:
            raise FixtureError(f'a job assertion needs job, state and attempts: {assertion!r}')
    elif 'call' in assertion:
        if assertion['call'] not in TOOLS or set(assertion) - {'call', 'arguments', 'error'} or \
                not isinstance(assertion.get('error'), str) or not assertion['error']:
            raise FixtureError(f'a call assertion needs a tool and an error code: {assertion!r}')
    elif 'stderr' in assertion:
        if assertion['stderr'] not in ('report', 'failure_only') or len(assertion) != 1:
            raise FixtureError(f'stderr is "report" or "failure_only": {assertion!r}')
    else:
        raise FixtureError(f'unknown assertion: {assertion!r}')


def payload(step, n):
    text = step['submit'].replace('{n}', str(n)) if n is not None else step['submit']
    return text.ljust(step['pad']) if 'pad' in step else text


def flatten(steps):
    """Each step with its repeat counter, in the order the steps run."""
    for step in steps:
        if step_kind(step) == 'repeat':
            start = step.get('from', 1)
            for n in range(start, start + step['repeat']):
                for inner in step.get('steps', []):
                    yield inner, n
        else:
            yield step, None


class Refused(Exception):
    """The service replied with an error the step did not expect."""


class Client:
    """Calls a running `mo serve` directly over MCP, at most 20 calls a second."""

    def __init__(self, url, token):
        parts = urllib.parse.urlsplit(url)
        if parts.hostname != '127.0.0.1':
            raise FixtureError(f'the service must be at 127.0.0.1: {url}')
        self.port, self.path, self.token = parts.port, parts.path or '/mcp', token
        self.next_id = 0
        self.calls = []  # (tool, seconds since the epoch, error code or None)

    def call(self, tool, **arguments):
        """The call's structured content and its runtime error code, or None when it worked."""
        self.next_id += 1
        body = json.dumps({'jsonrpc': '2.0', 'id': self.next_id, 'method': 'tools/call',
                           'params': {'name': tool, 'arguments': arguments}})
        time.sleep(PACE)
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=120)
        try:
            connection.request('POST', self.path, body, {
                'Authorization': f'Bearer {self.token}', 'Content-Type': 'application/json',
                'Accept': 'application/json, text/event-stream',
                'MCP-Protocol-Version': PROTOCOL})
            response = connection.getresponse()
            data = response.read()
        finally:
            connection.close()
        if response.status != 200:
            raise Refused(f'{tool}: HTTP {response.status}: {data[:200]!r}')
        result = json.loads(data)['result']
        content = result.get('structuredContent', {})
        code = None
        if result.get('isError'):
            error = content.get('error', {})
            code = error.get('code') or error.get('kind') or 'error'
        self.calls.append((tool, time.time(), code))
        return content, code

    def ok(self, tool, **arguments):
        content, code = self.call(tool, **arguments)
        if code is not None:
            raise Refused(f'{tool} {arguments}: {json.dumps(content)[:300]}')
        return content.get('value')

    def job(self, job_id):
        page = self.ok('list', filter={'case': 'AllJobs'}, after=str(job_id - 1), limit='1')
        jobs = page['jobs']
        if not jobs or jobs[0]['id'] != str(job_id):
            raise CheckFailed(f'job {job_id} is not in the queue')
        return jobs[0]

    def every_job(self):
        jobs, after = [], '0'
        while True:
            page = self.ok('list', filter={'case': 'AllJobs'}, after=after, limit='4')
            jobs += page['jobs']
            if page['next']['case'] == 'None':
                return jobs
            after = page['next']['value']


class Service:
    """`mo serve PROGRAM` on a data folder, with its standard error in a file."""

    def __init__(self, mo, program, data, token, stderr_path):
        self.stderr_path = pathlib.Path(stderr_path)
        self.stderr = open(self.stderr_path, 'wb')
        self.process = subprocess.Popen(
            [str(mo), 'serve', str(program), '--port', '0', '--data', str(data)],
            env={'PATH': os.environ.get('PATH', ''), 'QUEUE_TOKEN': token},
            stdout=subprocess.PIPE, stderr=self.stderr, stdin=subprocess.DEVNULL, bufsize=0)
        self.port = self.listening()
        self.url = f'http://127.0.0.1:{self.port}/mcp'

    def listening(self):
        line, limit = b'', time.monotonic() + 120
        while not line.endswith(b'\n'):
            if not select.select([self.process.stdout], [], [], limit - time.monotonic())[0]:
                self.process.kill()
                raise CheckFailed(f'mo serve printed no listening line: {line!r}')
            chunk = self.process.stdout.read(1)
            if not chunk:
                status = self.process.wait(timeout=60)
                raise CheckFailed(f'mo serve exited {status}: {self.stderr_text()[-500:]}')
            line += chunk
        return int(line.split(b':')[-1].split(b'/')[0])

    def stderr_text(self):
        if not self.stderr.closed:
            self.stderr.flush()
        return self.stderr_path.read_text(encoding='utf-8', errors='replace')

    def stop(self):
        self.process.send_signal(signal.SIGTERM)
        status = self.process.wait(timeout=120)
        self.process.stdout.close()
        self.stderr.close()
        return status


def replay(client, steps, log):
    """Sends the steps, in order and in real time."""
    last = None
    for step, n in flatten(steps):
        kind = step_kind(step)
        if kind == 'submit':
            client.ok('submit', payload=payload(step, n))
        elif kind == 'lease':
            leased = client.ok('lease')
            if leased['case'] != 'Some':
                raise CheckFailed('a lease step found no ready job')
            last = leased['value']
        elif kind in SETTLE:
            if last is None:
                raise FixtureError(f'{kind} before any lease')
            client.ok(kind, id=last['id'], attempt=last['attempts'])
        elif kind == 'wait':
            log.append({'wait': step['wait'], 'at': time.time()})
            time.sleep(step['wait'])
        elif kind == 'expect':
            holds(client, step['expect'], None)


def holds(client, assertion, stderr_text):
    """Raises CheckFailed, naming the assertion, when it does not hold."""
    if 'job' in assertion:
        job = client.job(assertion['job'])
        state, attempts = job['state']['case'], int(job['attempts'])
        if (state, attempts) != (assertion['state'], assertion['attempts']):
            raise CheckFailed(f'job {assertion["job"]} is {state} after {attempts} attempts, '
                              f'not {assertion["state"]} after {assertion["attempts"]}: '
                              f'{json.dumps(assertion)}')
    elif 'call' in assertion:
        content, code = client.call(assertion['call'], **assertion.get('arguments', {}))
        if code != assertion['error']:
            raise CheckFailed(f'{assertion["call"]} gave {code or "no error"}, not '
                              f'{assertion["error"]}: {json.dumps(content)[:300]}')
    elif 'stderr' in assertion:
        if stderr_text is None:
            raise CheckFailed('no standard error file was given for a stderr assertion')
        lines = stderr_text.splitlines()
        failures = [i for i, line in enumerate(lines) if line.startswith('mo serve: failure:')]
        if not failures:
            raise CheckFailed(f'standard error holds no failure line: {json.dumps(assertion)}')
        after = lines[failures[-1] + 1:failures[-1] + 2]
        reported = bool(after) and not after[0].startswith('mo serve:')
        if reported != (assertion['stderr'] == 'report'):
            raise CheckFailed(f'standard error {"has" if reported else "has no"} report after '
                              f'its failure line: {json.dumps(assertion)}')


def check(fixture, client, stderr_path):
    for assertion in fixture.get('check', []):
        text = None
        if 'stderr' in assertion and stderr_path is not None:
            time.sleep(0.5)  # the failure line is written as the reply is sent
            text = pathlib.Path(stderr_path).read_text(encoding='utf-8', errors='replace')
        holds(client, assertion, text)


def log_records(data):
    """The snapshot's sequence and each later log record as (sequence, tool, microseconds,
    arguments), where a tool is a name and arguments are the encoded values."""
    connection = sqlite3.connect(f'file:{pathlib.Path(data) / "service.sqlite"}?mode=ro',
                                 uri=True)
    try:
        snapshot = connection.execute('SELECT record FROM snapshot').fetchone()[0]
        rows = connection.execute('SELECT seq, record FROM log ORDER BY seq').fetchall()
    finally:
        connection.close()
    records = []
    for seq, record in rows:
        at = PAYLOAD_AT
        timer = record[at]
        index, arrival, count = struct.unpack('<IqI', record[at + 1:at + 17])
        at += 17 + 8  # the fields above, then the resulting state's checksum
        arguments = []
        for _ in range(count):
            (length,) = struct.unpack('<I', record[at:at + 4])
            arguments.append(record[at + 4:at + 4 + length])
            at += 4 + length
        records.append((seq, 'expire' if timer else TOOLS[index], arrival, arguments))
    return struct.unpack('<Q', snapshot[6:14])[0], records


def encoded_int(value):
    """An encoded Int argument's value: the last eight bytes of its encoding."""
    return struct.unpack('<q', value[-8:])[0]


def rfc3339(microseconds):
    seconds = microseconds // 1_000_000
    return time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(seconds))


def killing_call(data, job_id, margin):
    snapshot, records = log_records(data)
    if len(records) + margin > 100:
        raise CheckFailed(f'{len(records)} log rows follow the snapshot at {snapshot}, which '
                          f'leaves fewer than {margin} free')
    fails = [(tool, arrival) for _, tool, arrival, args in records
             if tool == 'fail' and len(args) == 2 and encoded_int(args[0]) == job_id and
             encoded_int(args[1]) == 3]
    if len(fails) != 1:
        raise CheckFailed(f'the log after the snapshot holds {len(fails)} third failures of '
                          f'job {job_id}, not one')
    tool, arrival = fails[0]
    return {'answer': f'{tool} {rfc3339(arrival)}', 'snapshot': snapshot,
            'rows_after_snapshot': len(records)}


def shared_payload_jobs(client):
    jobs = client.every_job()
    counts = {}
    for job in jobs:
        counts[job['payload']] = counts.get(job['payload'], 0) + 1
    return {'answer': str(sum(1 for job in jobs if counts[job['payload']] > 1)),
            'jobs': len(jobs)}


def key_live(fixture, client):
    rule = (fixture.get('key') or {}).get('rule')
    return shared_payload_jobs(client) if rule == 'shared_payload_jobs' else None


def key_stopped(fixture, data):
    key = fixture.get('key') or {}
    if key.get('rule') == 'killing_call':
        return killing_call(data, key['job'], key.get('margin', 60))
    return None


def mo_text(text):
    out = []
    for char in text:
        if char in '\\"#':
            out.append('\\' + char)
        elif char == '\n':
            out.append('\\n')
        elif char == '\t':
            out.append('\\t')
        elif char == '\r':
            out.append('\\r')
        elif ord(char) < 0x20 or ord(char) == 0x7f:
            out.append(f'\\u{{{ord(char):X}}}')
        else:
            out.append(char)
    return '"' + ''.join(out) + '"'


def mo_payload_text(step, counter):
    """The payload's text literal, with the repeat's counter interpolated, before padding."""
    parts = step['submit'].split('{n}')
    if counter is None or len(parts) == 1:
        return mo_text(step['submit'])
    return '"' + f'#{{{counter}}}'.join(mo_text(part)[1:-1] for part in parts) + '"'


HEADER = '''## Generated by learnability/phase4/fixture.py from fixture.json; do not edit.
## `replay` sends the fixture's steps into a simulation, each wait as an advance.
module FixtureSteps
expose replay
use Main
use JobQueue{Job}

fn submit(sim: Simulation, payload: Text) : Unit
  return Unit if Simulation.send(sim, Main.submit, payload) is Ok(Ok(_))
  stop "a fixture submit was refused"
end

fn lease(sim: Simulation) : Job
  return job if Simulation.send(sim, Main.lease) is Ok(Ok((_, Some(job))))
  stop "a fixture lease found no ready job"
end

fn acknowledge(sim: Simulation, job: Job) : Unit
  return Unit if Simulation.send(sim, Main.acknowledge, job.id, job.attempts) is Ok(Ok(_))
  stop "a fixture acknowledge was refused"
end

fn fail(sim: Simulation, job: Job) : Unit
  return Unit if Simulation.send(sim, Main.fail, job.id, job.attempts) is Ok(Ok(_))
  stop "a fixture fail was refused"
end

fn heartbeat(sim: Simulation, job: Job) : Unit
  return Unit if Simulation.send(sim, Main.heartbeat, job.id, job.attempts) is Ok(Ok(_))
  stop "a fixture heartbeat was refused"
end
'''


def submit_call(pad, step, counter):
    """A submit's lines, broken as `mo format` breaks a call that passes 100 columns."""
    text = mo_payload_text(step, counter)
    padding = f'.pad_right({step["pad"]})' if 'pad' in step else ''
    one_line = f'{pad}submit(sim, {text}{padding})'
    if len(one_line) <= 100:
        return [one_line]
    argument = [f'{pad}  {text}{padding}']
    if padding and len(argument[0]) > 100:
        argument = [f'{pad}  {text}', f'{pad}    {padding}']
    return [f'{pad}submit(', f'{pad}  sim,', *argument, f'{pad})']


def duration(seconds):
    """A whole number of seconds as `mo format` writes it."""
    count, unit = (seconds // 60, 'minute') if seconds % 60 == 0 else (seconds, 'second')
    return f'{count}.{unit}' if count == 1 else f'{count}.{unit}s'


def render(fixture):
    """The text of fixture-steps.mo."""
    lines = []
    leases = []  # the index in lines of each lease, in order
    used = set()

    def emit(steps, indent, counter):
        for step in steps:
            kind = step_kind(step)
            pad = '  ' * indent
            if kind == 'submit':
                lines.extend(submit_call(pad, step, counter))
            elif kind == 'lease':
                leases.append(len(lines))
                lines.append(f'{pad}_taken_{len(leases)} = lease(sim)')
            elif kind in SETTLE:
                if not leases:
                    raise FixtureError(f'{kind} before any lease')
                used.add(leases[-1])
                lines.append(f'{pad}{kind}(sim, taken_{len(leases)})')
            elif kind == 'wait':
                lines.append(f'{pad}Simulation.advance(sim, {duration(step["wait"])})')
            elif kind == 'repeat':
                start = step.get('from', 1)
                body = step.get('steps', [])
                uses = any(step_kind(s) == 'submit' and '{n}' in s['submit'] for s in body)
                name = 'n' if uses else '_n'
                lines.append(f'{pad}for {name} in {start}..{start + step["repeat"]}')
                emit(body, indent + 1, 'n' if uses else None)
                lines.append(f'{pad}end')

    emit(fixture['steps'], 1, None)
    for index in used:  # a lease the steps act on is named without the unused mark
        lines[index] = lines[index].replace('_taken_', 'taken_', 1)
    body = lines + ['  Unit']
    return HEADER + '\n## The fixture\'s pre-state.\nfn replay(sim: Simulation) : Unit\n' + \
        '\n'.join(body) + '\nend\n'


def rendered(variant):
    """Whether VARIANT/fixture-steps.mo is what its fixture.json renders to."""
    target = pathlib.Path(variant) / 'fixture-steps.mo'
    return target.exists() and target.read_text(encoding='utf-8') == render(load(variant))


def token_from(env_name):
    token = os.environ.get(env_name)
    if not token:
        raise FixtureError(f'the environment variable {env_name} holds no token')
    return token


def build(variant, mo, out, token_env):
    fixture = load(variant)
    token = token_from(token_env)
    out = pathlib.Path(out)
    data = out / 'data'
    if data.exists():
        raise FixtureError(f'{data} already exists')
    data.mkdir(parents=True)
    log = []
    service = Service(mo, pathlib.Path(variant) / 'main.mo', data, token, out / 'build-stderr')
    client = Client(service.url, token)
    started = time.time()
    try:
        replay(client, fixture['steps'], log)
        check(fixture, client, out / 'build-stderr')
        key = key_live(fixture, client)
    finally:
        status = service.stop()
    if status != 0:
        raise CheckFailed(f'mo serve exited {status} when stopped')
    key = key or key_stopped(fixture, data)
    failing = fixture.get('failing_tool')
    record = {'calls': len(client.calls), 'seconds': round(time.time() - started, 1),
              'failing_tool_calls': [at for tool, at, code in client.calls
                                     if tool == failing and code is not None],
              'waits': log}
    (out / 'build.json').write_text(json.dumps(record, indent=2) + '\n')
    if key is not None:
        (out / 'key.json').write_text(json.dumps(key, indent=2) + '\n')
    return record


def key_from(variant, mo, data, token_env):
    fixture = load(variant)
    if fixture.get('key') is None:
        raise FixtureError(f'{variant} is not a question')
    if fixture['key']['rule'] == 'killing_call':
        return key_stopped(fixture, data)
    token = token_from(token_env)
    with tempfile.TemporaryDirectory() as temporary:
        copy = pathlib.Path(temporary) / 'data'
        shutil.copytree(data, copy)
        service = Service(mo, pathlib.Path(variant) / 'main.mo', copy, token,
                          pathlib.Path(temporary) / 'stderr')
        try:
            return key_live(fixture, Client(service.url, token))
        finally:
            service.stop()


def main(args):
    try:
        if len(args) == 5 and args[0] == 'build':
            record = build(args[1], args[2], args[3], args[4])
            print(f'built {args[1]}: {record["calls"]} calls in {record["seconds"]} s')
        elif len(args) == 2 and args[0] in ('render', 'rendered'):
            target = pathlib.Path(args[1]) / 'fixture-steps.mo'
            if args[0] == 'render':
                target.write_text(render(load(args[1])), encoding='utf-8')
            elif not rendered(args[1]):
                print(f'{target} is not what fixture.json renders to', file=sys.stderr)
                return 1
        elif len(args) in (4, 5) and args[0] == 'check':
            fixture = load(args[1])
            check(fixture, Client(args[2], token_from(args[3])),
                  args[4] if len(args) == 5 else None)
            print(f'checked {args[1]}: every assertion holds')
        elif len(args) == 5 and args[0] == 'key':
            print(json.dumps(key_from(args[1], args[2], args[3], args[4]), indent=2))
        else:
            print(__doc__.split('\n\n')[1], file=sys.stderr)
            return 2
    except FixtureError as error:
        print(f'fixture: {error}', file=sys.stderr)
        return 2
    except (CheckFailed, Refused) as error:
        print(f'check failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
