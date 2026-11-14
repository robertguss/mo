#!/usr/bin/env python3
"""The task queue: its simulation tests, then the real program over loopback."""
import collections
import datetime
import http.client
import json
import os
import pathlib
import select
import signal
import subprocess
import sys
import tempfile
import time


FOLDER = 'services/task-queue'
TOKEN = 'TaskQueueCanary42'
JOB_QUEUE_RESULTS = 26
QUEUE_TEST_RESULTS = 23
MAIN_RESULTS = 9
SERVICE_CHECKS = [*(f'Main.service.promise.{n}' for n in range(1, 11)), 'Main.service.recovery']
DESCRIPTION_LIMIT = 4096
INT = {'type': 'string', 'pattern': '^-?[0-9]+$'}
INITIALIZE = {'jsonrpc': '2.0', 'id': 1, 'method': 'initialize', 'params': {
    'protocolVersion': '2025-11-25', 'capabilities': {},
    'clientInfo': {'name': 'check', 'version': '1'}}}
ALL_JOBS = {'case': 'AllJobs'}


class Queue:
    """One mo serve process and a client that keeps under the token's rate limit."""

    def __init__(self, binary, data):
        self.process = subprocess.Popen(
            [str(binary), 'serve', f'{FOLDER}/main.mo', '--port', '0', '--data', str(data)],
            env={**os.environ, 'QUEUE_TOKEN': TOKEN}, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=0)
        try:
            self.port = self.listening()
        except BaseException:
            # A server that never says it is listening is still running; don't orphan it.
            self.kill()
            raise
        self.next_id = 100

    def listening(self):
        line = b''
        limit = time.monotonic() + 30
        while not line.endswith(b'\n'):
            assert select.select([self.process.stdout], [], [], limit - time.monotonic())[0], line
            chunk = self.process.stdout.read(1)
            assert chunk, (line, self.process.stderr.read())
            line += chunk
        prefix, suffix = b'mo serve: listening on http://127.0.0.1:', b'/mcp\n'
        assert line.startswith(prefix) and line.endswith(suffix), line
        return int(line[len(prefix):-len(suffix)])

    def post(self, body, token=TOKEN):
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=30)
        connection.request('POST', '/mcp', body, {
            'Authorization': f'Bearer {token}', 'Content-Type': 'application/json',
            'MCP-Protocol-Version': '2025-11-25'})
        response = connection.getresponse()
        data = response.read()
        connection.close()
        return response.status, data

    def rpc(self, method, params=None, request_id=None):
        self.next_id += 1
        message = {'jsonrpc': '2.0', 'id': self.next_id if request_id is None else request_id,
                   'method': method}
        if params is not None:
            message['params'] = params
        status, body = self.post(json.dumps(message))
        assert status == 200, (status, body)
        return json.loads(body)

    def call(self, tool, request_id=None, **arguments):
        """A tool call's structuredContent and isError, after pacing to the rate limit."""
        time.sleep(0.06)
        result = self.rpc('tools/call', {'name': tool, 'arguments': arguments}, request_id)['result']
        assert len(result['content']) == 1 and result['content'][0]['type'] == 'text', result
        assert json.loads(result['content'][0]['text']) == result['structuredContent'], result
        return result['structuredContent'], result['isError']

    def ok(self, tool, **arguments):
        content, failed = self.call(tool, **arguments)
        assert not failed, (tool, content)
        return content['value']

    def job(self, id):
        return self.ok('list', filter=ALL_JOBS, after=str(int(id) - 1), limit='1')['jobs'][0]

    def every_job(self):
        """Read as the list tool tells agents to: halve the limit on response_limit."""
        jobs, after, limit = [], '0', 100
        while True:
            content, failed = self.call('list', filter=ALL_JOBS, after=after, limit=str(limit))
            if failed:
                assert content['error']['code'] == 'response_limit' and limit > 1, content
                limit //= 2
                continue
            jobs += content['value']['jobs']
            if content['value']['next']['case'] == 'None':
                return jobs
            after = content['value']['next']['value']

    def stop(self):
        self.process.send_signal(signal.SIGTERM)
        assert self.process.wait(timeout=30) == 0, self.process.stderr.read()
        self.close()

    def kill(self):
        self.process.kill()
        self.process.wait(timeout=30)
        self.close()

    def close(self):
        self.process.stdout.close()
        self.process.stderr.close()


def simulation(binary):
    result = subprocess.run([str(binary), 'test', '--json', f'{FOLDER}/queue-tests.mo'],
                            capture_output=True, text=True, timeout=1800)
    assert result.returncode == 0 and not result.stderr, result
    records = [json.loads(line) for line in result.stdout.splitlines()]
    tests = [r for r in records if r['record'] == 'result']
    assert all(r['outcome'] == 'passed' for r in tests), tests
    modules = collections.Counter(r['name'].split('.')[0] for r in tests)
    assert modules == {'JobQueue': JOB_QUEUE_RESULTS, 'QueueTests': QUEUE_TEST_RESULTS,
                       'Main': MAIN_RESULTS + len(SERVICE_CHECKS)}, modules
    assert [r['name'] for r in tests[-len(SERVICE_CHECKS):]] == SERVICE_CHECKS, tests
    assert tests[-1]['kind'] == 'recovery', tests


EXPIRE = '''      after = release(after, job)
    end
  end
  after
end
'''
PLANTED = '''      after = release(after, job)
    end
  end
  return after if (now - t0).in_milliseconds % 60000 == 0
  after.jobs = after.jobs.remove(after.next_id - 1)
  after
end
'''


def restart_fault(binary):
    """A planted fault that loses the newest job only at a restart's startup tick, after the clock
    moved: recovery itself is exact, so only the promise that no job is lost catches it, with a
    short sequence that ends at the restart."""
    with tempfile.TemporaryDirectory(prefix='mo-task-queue-fault-') as directory:
        folder = pathlib.Path(directory)
        for file in pathlib.Path(FOLDER).glob('*.mo'):
            (folder / file.name).write_text(file.read_text())
        source = (folder / 'job-queue.mo').read_text()
        assert source.count(EXPIRE) == 1
        (folder / 'job-queue.mo').write_text(source.replace(EXPIRE, PLANTED))
        result = subprocess.run([str(binary), 'test', '--json', str(folder / 'main.mo')],
                                capture_output=True, text=True, timeout=1800)
        assert result.returncode == 1 and not result.stderr, result
        records = {r['name']: r for r in map(json.loads, result.stdout.splitlines())
                   if r['record'] == 'result'}
        assert records['Main.service.recovery']['outcome'] == 'passed', records
        lost = records['Main.service.promise.1']
        assert lost['outcome'] == 'failed', lost
        actions = [step['action'] for step in lost['steps']]
        assert len(actions) <= 6 and actions[-2:] == ['crash', 'restart'], lost
        assert lost['steps'][-1]['broke'] == 'Main.service.promise.1', lost


def catalog(queue):
    assert queue.rpc('initialize', INITIALIZE['params'])['result']['protocolVersion'] == '2025-11-25'
    assert queue.post('{"jsonrpc":"2.0","method":"notifications/initialized"}')[0] == 202
    tools, cursor = [], None
    while True:
        page = queue.rpc('tools/list', {'cursor': cursor} if cursor else None)['result']
        tools += page['tools']
        cursor = page.get('nextCursor')
        if cursor is None:
            break
    assert [tool['name'] for tool in tools] == ['submit', 'lease', 'heartbeat', 'acknowledge',
                                                'fail', 'list', 'stats', 'mo-overview', 'mo-state',
                                                'mo-mailbox', 'mo-timers', 'mo-memory',
                                                'mo-supervisor', 'mo-log', 'mo-export', 'mo-failures',
                                                'mo-upgrade-open', 'mo-upgrade-page',
                                                'mo-upgrade-commit'], tools
    for tool in tools:
        assert 0 < len(tool['description']) <= DESCRIPTION_LIMIT, tool['name']
    schemas = {tool['name']: tool['inputSchema'] for tool in tools}
    lease = {'id': INT, 'attempt': INT}
    for name, properties in (('submit', {'payload': {'type': 'string'}}), ('lease', {}),
                             ('stats', {}),
                             ('heartbeat', lease), ('acknowledge', lease), ('fail', lease)):
        schema = schemas[name]
        assert schema['type'] == 'object' and schema['additionalProperties'] is False, schema
        assert schema['properties'] == properties, schema
        assert sorted(schema.get('required', [])) == sorted(properties), schema
    listing = schemas['list']
    assert sorted(listing['required']) == ['after', 'filter', 'limit'], listing
    assert listing['properties']['after'] == INT and listing['properties']['limit'] == INT, listing
    filters = listing['$defs'][listing['properties']['filter']['$ref'].rsplit('/', 1)[1]]['oneOf']
    assert [choice['properties']['case']['const'] for choice in filters] == [
        'AllJobs', 'Ready', 'Leased', 'Done', 'Dead'], listing


def parsed(text):
    return datetime.datetime.fromisoformat(text.replace('Z', '+00:00'))


def lifecycle(queue):
    assert queue.ok('lease') == {'case': 'None'}
    assert queue.ok('submit', payload='write the runbook') == '1'
    before = datetime.datetime.now(datetime.timezone.utc)
    taken = queue.ok('lease')
    after = datetime.datetime.now(datetime.timezone.utc)
    until = taken['value']['state']['fields']['until']
    assert before + datetime.timedelta(minutes=30) - datetime.timedelta(seconds=1) <= parsed(until)
    assert parsed(until) <= after + datetime.timedelta(minutes=30), until
    assert taken == {'case': 'Some', 'value': {
        'id': '1', 'payload': 'write the runbook', 'attempts': '1',
        'state': {'case': 'Leased', 'fields': {'until': until}}}}, taken
    assert queue.ok('heartbeat', id='1', attempt='1') is None
    assert queue.ok('acknowledge', id='1', attempt='1') is None
    assert queue.job('1') == {'id': '1', 'payload': 'write the runbook', 'attempts': '1',
                              'state': {'case': 'Done'}}
    counted = queue.ok('stats')
    assert sorted(counted) == ['dead', 'done', 'leased', 'limit', 'now', 'ready', 'total'], counted
    assert {key: value for key, value in counted.items() if key != 'now'} == {
        'total': '1', 'ready': '0', 'leased': '0', 'done': '1', 'dead': '0', 'limit': '5000'}, counted
    page = queue.ok('list', filter={'case': 'Done'}, after='0', limit='100')
    assert sorted(page) == ['jobs', 'next', 'now'] and page['next'] == {'case': 'None'}, page
    assert abs(parsed(page['now']) - datetime.datetime.now(datetime.timezone.utc)) < \
        datetime.timedelta(seconds=30), page

    content, failed = queue.call('acknowledge', id='1', attempt='1')
    assert failed and content == {'error': {'kind': 'application', 'value': {
        'case': 'NotLeased', 'fields': {'id': '1'}}}}, content
    content, failed = queue.call('heartbeat', id='9', attempt='1')
    assert failed and content == {'error': {'kind': 'application', 'value': {
        'case': 'NoSuchJob', 'fields': {'id': '9'}}}}, content
    content, failed = queue.call('list', filter=ALL_JOBS, after='0', limit='0')
    assert failed and content == {'error': {'kind': 'application', 'value': {
        'case': 'BadPage', 'fields': {'after': '0', 'limit': '0', 'max_limit': '100'}}}}, content


def size_limits(queue):
    worst = '\x01' * 2048
    id = queue.ok('submit', payload=worst)
    longest = '\x01' * 128
    content, failed = queue.call('lease', request_id=longest)
    assert not failed and content['value']['value']['payload'] == worst, content
    content, failed = queue.call('list', request_id=longest, filter=ALL_JOBS,
                                 after=str(int(id) - 1), limit='1')
    assert not failed and content['value']['jobs'][0]['payload'] == worst, content
    assert queue.ok('acknowledge', id=id, attempt='1') is None

    content, failed = queue.call('submit', payload='x' * 2049)
    assert failed and content == {'error': {'kind': 'application', 'value': {
        'case': 'PayloadTooLarge', 'fields': {'limit': '2048', 'bytes': '2049'}}}}, content
    jobs = queue.ok('list', filter=ALL_JOBS, after='0', limit='100')['jobs']

    time.sleep(0.06)
    status, body = queue.post(json.dumps({
        'jsonrpc': '2.0', 'id': 3, 'method': 'tools/call',
        'params': {'name': 'submit', 'arguments': {'payload': 'x' * (17 << 10)}}}))
    assert status == 413 and not body, (status, body)
    assert queue.ok('list', filter=ALL_JOBS, after='0', limit='100')['jobs'] == jobs


def paging(queue):
    first = queue.ok('submit', payload='x' * 2048)
    for _ in range(9):
        queue.ok('submit', payload='x' * 2048)
    content, failed = queue.call('list', filter={'case': 'Ready'}, after='0', limit='100')
    assert failed, content
    error = content['error']
    assert error['kind'] == 'runtime' and error['code'] == 'response_limit', error
    assert error['effect'] == 'none', error
    page = queue.ok('list', filter={'case': 'Ready'}, after='0', limit='5')
    assert [job['id'] for job in page['jobs']][0] == first and len(page['jobs']) == 5, page
    assert page['next'] == {'case': 'Some', 'value': page['jobs'][-1]['id']}, page


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    simulation(binary)
    restart_fault(binary)
    with tempfile.TemporaryDirectory(prefix='mo-task-queue-') as directory:
        data = pathlib.Path(directory)
        queue = Queue(binary, data)
        try:
            catalog(queue)
            lifecycle(queue)
            size_limits(queue)
            paging(queue)
            status, body = queue.post('{"jsonrpc":"2.0","id":4,"method":"tools/list"}', 'WrongCanary42')
            assert status == 401 and b'tools' not in body, (status, body)

            accepted = queue.ok('submit', payload='survive the kill')
            before = queue.every_job()
        except BaseException:
            queue.kill()
            raise
        queue.kill()

        queue = Queue(binary, data)
        try:
            assert queue.every_job() == before
            assert queue.job(accepted)['payload'] == 'survive the kill'
            while True:
                taken = queue.ok('lease')
                assert taken['case'] == 'Some', 'the accepted job was never leased'
                if taken['value']['id'] == accepted:
                    break
            assert taken['value']['payload'] == 'survive the kill', taken
        except BaseException:
            queue.kill()
            raise
        queue.stop()
    print('Task queue: simulation, a restart fault, wire shapes, size limits, paging and recovery '
          'after kill -9 pass')


if __name__ == '__main__':
    main()
