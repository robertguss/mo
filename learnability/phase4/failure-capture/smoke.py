#!/usr/bin/env python3
"""Development-only failure capture/view/fetch/snapshot/final-stop trace."""
import base64
import http.client
import json
import os
import pathlib
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

mo = str(pathlib.Path(sys.argv[1]).resolve())
result = pathlib.Path(sys.argv[2])
result.mkdir(parents=True, exist_ok=True)
transcript = []
with tempfile.TemporaryDirectory() as temporary:
    root = pathlib.Path(temporary)
    (root / 'data').mkdir()
    source = root / 'main.mo'
    source.write_text('''module Main
service
state Text = "x".repeat(20000)
tools broken, add
token from env "CAPTURE_TEST_TOKEN"
end
fn broken(_state: Text, _now: Time, _n: Int) : Bool
stop "capture smoke"
end
fn add(state: Text, _now: Time) : (Text, Bool)
("#{state}x", true)
end
''')
    sock = socket.socket()
    sock.bind(('127.0.0.1', 0))
    port = sock.getsockname()[1]
    sock.close()
    env = dict(os.environ, CAPTURE_TEST_TOKEN='CaptureDevelopmentCanary')
    errors = open(result / 'serve.stderr', 'w')
    proc = subprocess.Popen([mo, 'serve', str(source), '--port', str(port), '--data', str(root / 'data')], env=env, stdout=subprocess.DEVNULL, stderr=errors)
    def call(name, arguments):
        time.sleep(.06)
        connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
        body = json.dumps({'jsonrpc':'2.0','id':1,'method':'tools/call','params':{'name':name,'arguments':arguments}})
        connection.request('POST', '/mcp', body, {'Authorization':'Bearer CaptureDevelopmentCanary','Content-Type':'application/json','MCP-Protocol-Version':'2025-11-25'})
        response = connection.getresponse()
        value = json.loads(response.read())
        connection.close()
        transcript.append({'tool':name,'arguments':arguments,'status':response.status,'response':value})
        return value
    try:
        for _ in range(200):
            try:
                sock = socket.create_connection(('127.0.0.1',port),timeout=.2)
                sock.close()
                break
            except OSError:
                assert proc.poll() is None
                time.sleep(.05)
        call('broken', {'_n':'0'})
        view = call('mo-failures', {})
        assert '"id": "1"' in json.dumps(view), view
        value = view['result']['structuredContent']['value']
        assert value['truncated'] == 0 and value['records'][0]['signature']['class'] == 'defect', value
        assert value['records'][0]['signature']['version'] == 1, value
        page = call('mo-export', {'kind':'failure','failure':1})['result']['structuredContent']['value']
        assert page['total'] > 8192 and page['next']['failure'] == 1, page
        parts = [base64.b64decode(page['base64'])]
        wrong = dict(page['next'], failure=2)
        refused = call('mo-export', wrong)
        assert refused['result']['isError'], refused
        while page['next'] is not None:
            page = call('mo-export', page['next'])['result']['structuredContent']['value']
            parts.append(base64.b64decode(page['base64']))
        assert len(parts) > 1 and sum(map(len, parts)) == page['total']
        exported = root / 'failure.moex' 
        fetch = subprocess.run([mo,'fetch',f'http://127.0.0.1:{port}/mcp','--export','failure','1','--out',str(exported),'--token-env','CAPTURE_TEST_TOKEN'],env=env,capture_output=True,timeout=120)
        (result/'fetch.stderr').write_bytes(fetch.stderr)
        assert fetch.returncode == 0, fetch.stderr
        assert exported.stat().st_mode & 0o777 == 0o600
        assert exported.read_bytes()[:6] == b'MOEX\x01\x02'
        for _ in range(101):
            call('add', {})
        for _ in range(14):
            call('broken', {'_n':'0'})
        proc.wait(timeout=20)
        assert proc.returncode == 69, proc.returncode
        errors.flush()
        text = (result/'serve.stderr').read_text()
        assert text.count('mo serve: failure:') == 15, text
        assert 'record: 15' in text and text.count('mo serve: stop:') == 1, text
        db = sqlite3.connect(root/'data/service.sqlite')
        assert db.execute('PRAGMA user_version').fetchone() == (1,)
        rows = db.execute('SELECT id FROM failure ORDER BY id').fetchall()
        assert rows == [(1,),(15,)], rows
        assert db.execute('SELECT count(*) FROM failure_state').fetchone() == (2,)
        db.close()
        (result/'summary.json').write_text(json.dumps({'failures':15,'records':rows,'mode':'0600','export_bytes':exported.stat().st_size,'direct_pages':len(parts),'status':'pass'},indent=2)+'\n')
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=20)
        errors.close()
        (result/'transcript.json').write_text(json.dumps(transcript,indent=2)+'\n')
