import contextlib
import json
import os
import pathlib
import shutil
import signal
import socket
import sqlite3
import stat
import subprocess
import tempfile
import time

from cli_common import (
    SERVE_ADD, SERVE_COUNTER, SERVE_EDITS, SERVE_FORGED, SERVE_FORGING, SERVE_HALT, SERVE_HALTING,
    SERVE_INITIALIZE, SERVE_INITIALIZED, SERVE_LIST, SERVE_OPERATOR_NAMES, SERVE_OVERLOADED,
    SERVE_READ, SERVE_REVIEW_NAMES, SERVE_SIGNALLED, SERVE_TOOL_PAGE, SERVE_UPGRADE_NAMES,
    STORE_BUSY, STORE_FAILED, Served, records, root, run, serve_events, serve_spellings,
    serve_structured, serve_supervised,
)


def service_commands(binary):
    with tempfile.TemporaryDirectory(prefix='mo-service-') as directory:
        folder = pathlib.Path(directory)
        path = folder / 'counter.mo'
        source = ('module Counter\nexpose snapshots\n\nservice\n'
                  '  state Int = stop "service initializer ran"\n'
                  '  tools snapshots\n'
                  '  every 1.minute tick\n'
                  '  token from env "MO_SERVICE_TEST_TOKEN_NOT_READ"\nend\n\n'
                  'fn snapshots(state: Int, _now: Time) : List(Int)\n'
                  '  [state]\nend\n\n'
                  'fn tick(state: Int, _now: Time) : Int\n  state + 1\nend\n\n'
                  'test "ordinary handlers"\n'
                  '  now = Time.parse("2026-10-25T00:00:00Z") or stop "valid time"\n'
                  '  assert snapshots(7, now) == [7]\n'
                  '  assert tick(7, now) == 8\nend\n')
        env = os.environ.copy()
        env.pop('MO_SERVICE_TEST_TOKEN_NOT_READ', None)
        path.write_text(source)
        for command in ('check', 'format', 'fix'):
            result = run(binary, [command, str(path)], 0, env=env)
            assert not result.stdout and not result.stderr, result
            assert path.read_text() == source
        # Tests call the handlers as ordinary functions. Only the recovery check starts the
        # service, so it alone meets the stopping start value; the token is never read.
        items = records(run(binary, ['test', '--json', str(path)], 1, env=env), 'test')
        assert [(r['name'], r['outcome']) for r in items[1:-1]] == [
            ('Counter.test.1', 'passed'), ('Counter.service.recovery', 'stopped')]
        assert items[2]['defect']['detail'] == 'service initializer ran', items[2]
        result = run(binary, ['run', str(path)], 1, env=env)
        assert not result.stdout
        assert b'error E1408: mo run does not start services yet' in result.stderr
        emitted = folder / 'counter.bc'
        result = run(binary, ['emit', str(path), '-o', str(emitted)], 0, env=env)
        assert not result.stdout and not result.stderr
        assert emitted.read_bytes().startswith(b'MOBC')
        result = run(binary, ['check', '--emit=bytecode', str(path)], 0, env=env)
        assert b'\nservices\n  S0 ' in result.stdout and not result.stderr
        path.write_text(source + '\nfn main()\nend\n')
        result = run(binary, ['emit', str(path), '-o', str(emitted)], 0, env=env)
        assert not result.stdout and not result.stderr
        assert emitted.read_bytes().startswith(b'MOBC')
        assert int.from_bytes(emitted.read_bytes()[8:12], 'little') == 1
        run(binary.parent / 'tests/test_cli.test', ['--run-bytecode', str(emitted)], 0)
        result = run(binary, ['check', '--emit=bytecode', str(path)], 0, env=env)
        assert b'\nservices\n  S0 ' in result.stdout and not result.stderr
        assert b'MO_SERVICE_TEST_TOKEN_NOT_READ' in result.stdout
        indexed = folder / 'counter.bytecode'
        indexed.write_bytes(result.stdout)
        run(binary.parent / 'tests/test_cli.test', ['--roundtrip-bytecode', str(indexed)], 0)
        result = run(binary, ['check', '--emit=core', str(path)], 0, env=env)
        assert b'"Counter.%service.start"' in result.stdout and not result.stderr
        assert b'Counter.%service.interval.1' in result.stdout
        assert b'MO_SERVICE_TEST_TOKEN_NOT_READ' not in result.stdout
        path.write_text(source)
        main = folder / 'main.mo'
        main.write_text('module Main\nuse Counter { snapshots }\n'
                        'fn main(console: Console)\n'
                        '  now = Time.parse("2026-10-25T00:00:00Z") or stop "valid time"\n'
                        '  console.print("#{snapshots(9, now)}")\nend\n')
        result = run(binary, ['run', str(main)], 0, env=env)
        assert result.stdout == b'[9]\n' and not result.stderr
        result = run(binary, ['check', '--emit=bytecode', str(main)], 0, env=env)
        assert b'\nservices\n  S0 ' in result.stdout and not result.stderr

        main.write_text('module Main\nuse Counter { snapshots }\n'
                        'struct First\nvalue: Int\nend\nstruct Second\nvalue: Bool\nend\n'
                        'service\nstate Second = stop "not started"\ntools snapshot\n'
                        'token from env "SECOND_TOKEN_NOT_READ"\nend\n'
                        'fn snapshot(state: First, _now: Time) : List(First)\n[state]\nend\n'
                        'fn snapshot(state: Second, _now: Time) : List(Second)\n[state]\nend\n'
                        'fn main(console: Console)\n'
                        '  now = Time.parse("2026-10-25T00:00:00Z") or stop "valid time"\n'
                        '  console.print("#{snapshots(9, now)}")\nend\n')
        result = run(binary, ['check', '--emit=bytecode', str(main)], 0, env=env)
        services = result.stdout.split(b'\nservices\n')[1].splitlines()
        assert services[0].startswith(b'  S0 ') and services[1].startswith(b'  S1 ')
        assert services[2] == b'end' and len(services) == 3
        indexed.write_bytes(result.stdout)
        run(binary.parent / 'tests/test_cli.test', ['--roundtrip-bytecode', str(indexed)], 0)
        run(binary, ['emit', str(main), '-o', str(emitted)], 0, env=env)
        result = run(binary.parent / 'tests/test_cli.test', ['--run-bytecode', str(emitted)], 0)
        assert result.stdout == b'[9]\n' and not result.stderr

        bare = (root / 'tests/errors/e0101/main.mo').read_text()
        path = main
        path.write_text(bare.replace('expose increment\n', ''))
        before = path.read_bytes()
        items = records(run(binary, ['check', '--json', str(path)], 1), 'check')
        assert len(items) == 3 and items[1]['code'] == 'E0101', items
        diagnostic = items[1]
        assert diagnostic['headline'] == 'a service tool returns only its state'
        assert diagnostic['primary']['label'] == \
            'a timer returns this; a tool returns (Int, reply)'
        assert diagnostic['reason'] == (
            'A tool changes the state only by returning (state, reply). Returning the state '
            "alone is how a timer changes it, so a tool's result would never be saved "
            '(reference 11.5).')
        assert [fix['title'] for fix in diagnostic['fixes']] == [
            'return (state, Unit) to save the new state',
            'return the state inside a record, as read-only data']
        assert all(fix['edits'] for fix in diagnostic['fixes'])
        result = run(binary, ['fix', str(path)], 1)
        assert not result.stdout and result.stderr.count(b'error E0101:') == 1
        assert path.read_bytes() == before
        path.write_text(bare)
        items = records(run(binary, ['check', '--json', str(path)], 1), 'check')
        assert items[1]['code'] == 'E0101' and items[1]['fixes'] == []
        run(binary, ['fix', str(path)], 1)
        assert path.read_text() == bare

        for case, label in (
            ('e0101-predicate', 'a timer returns this; a tool returns (Bool, reply)'),
            ('e0101-result', 'a tool returns Result((Int, reply), E)'),
        ):
            source = (root / 'tests/errors' / case / 'main.mo').read_text()
            path.write_text(source)
            items = records(run(binary, ['check', '--json', str(path)], 1), 'check')
            assert len(items) == 3 and items[1]['code'] == 'E0101', items
            assert items[1]['primary']['label'] == label and items[1]['fixes'] == []
            run(binary, ['fix', str(path)], 1)
            assert path.read_text() == source

        (folder / 'job-queue.mo').write_text((root / 'examples/job-queue.mo').read_text())
        for handler in ('heartbeat', 'acknowledge', 'fail'):
            path.write_text('module Main\nuse JobQueue\n'
                            'service\n  state JobQueue.Queue = JobQueue.empty\n'
                            '  tools change\n  token from env "TOKEN"\nend\n'
                            'fn change(state: JobQueue.Queue, now: Time, id: Int, attempt: Int)'
                            ' : Result(JobQueue.Queue, JobQueue.QueueError)\n'
                            f'  JobQueue.{handler}(state, now, id, attempt)\nend\n')
            items = records(run(binary, ['check', '--json', str(path)], 1), 'check')
            assert len(items) == 3 and items[1]['code'] == 'E0101', items
            assert items[1]['primary']['label'] == 'a tool returns Result((JobQueue.Queue, reply), E)'
            assert items[1]['fixes'] == []
            source = path.read_text()
            run(binary, ['fix', str(path)], 1)
            assert path.read_text() == source


def service_refusals(binary):
    source = ('module Main\nservice\n  state Int = 0\n  tools snapshot\n'
              '  every 1.minute tick\n  token from env "TOKEN"\nend\n'
              'fn snapshot(state: Int, _now: Time) : List(Int)\n  [state]\nend\n'
              'fn tick(state: Int, _now: Time) : Int\n  state\nend\n')
    cases = [
        ('state Int = 0', 'state Int = 0\n  state Int = 1', 'E0099'),
        ('  tools snapshot\n', '', 'E0099'),
        ('  token from env "TOKEN"\n', '', 'E0099'),
        ('tools snapshot', 'tools snapshot, snapshot', 'E0099'),
        ('1.minute tick', '1.minute snapshot', 'E0099'),
        ('_now: Time) : List(Int)', '_now: Int) : List(Int)', 'E0100'),
        ('fn tick(state: Int, _now: Time)', 'fn tick(state: Int, _now: Time, _extra: Int)',
         'E0100'),
        ('fn tick(state: Int, _now: Time) : Int\n  state',
         'fn tick(state: Int, _now: Time) : List(Int)\n  [state]', 'E0100'),
    ]
    cases += [('"TOKEN"', json.dumps(name, ensure_ascii=False), 'E0102')
              for name in ('', 'lowercase', '1TOKEN', 'TOKEN-NAME', 'TÖKEN')]
    cases += [('1.minute', interval, 'E0103')
              for interval in ('0.seconds', '-1.seconds', '1.5.seconds', '(1.minute)',
                               'Duration.seconds(1)', '1.minute + 1.minute')]
    cases += [('_now: Time) : List(Int)', f'_now: Time, _value: {type_}) : List(Int)',
               'E0104') for type_ in ('Option(Console)', 'List(fn() : Int)')]
    with tempfile.TemporaryDirectory(prefix='mo-service-refusals-') as directory:
        path = pathlib.Path(directory) / 'main.mo'
        for old, new, code in cases:
            path.write_text(source.replace(old, new))
            items = records(run(binary, ['check', '--json', str(path)], 1), 'check')
            assert [r['code'] for r in items[1:-1]] == [code], (new, items)
        for name in ('TOKEN', '_TOKEN_2', 'T0', '_'):
            path.write_text(source.replace('"TOKEN"', json.dumps(name)))
            run(binary, ['check', str(path)], 0)


def serve_guide():
    guide = (root / "docs/agent-guide.md").read_text(encoding="utf-8")
    for sentence in (
            "`mo serve FILE --port N --data DIR [--allow-host NAME]` runs the service that the "
            "module in `FILE` declares.",
            "`--allow-host NAME` also accepts requests whose `Host` is `NAME`",
            "A `NAME` without a port means port 80.",
            "The flag never changes which saved data the program recovers.",
            "`DIR` exists. `mo serve` never creates it",
            "Saved state goes in `DIR/service.sqlite`.",
            "Only one `mo serve` can use a `DIR` at a time.",
            "A missing or invalid token is E1409, and the message names the variable.",
            "`mo serve` listens on 127.0.0.1 only.",
            "mo serve: listening on http://127.0.0.1:PORT/mcp",
            "Any other well-formed `Host` gets status 421, and a malformed or missing one gets 400.",
            SERVE_COUNTER,
            "A missing or wrong token gets status 401.",
            "-H 'Content-Type: application/json'",
            "-H 'MCP-Protocol-Version: 2025-11-25'",
            f"-d '{SERVE_INITIALIZE}'",
            SERVE_INITIALIZED, SERVE_LIST, SERVE_ADD, SERVE_READ,
            "An `Int` argument is a JSON string",
            "mo serve: failure: NAME: REASON\nmo serve: restart: N of M: REASON\n"
            "mo serve: stop: REASON\n",
            "`NAME` is the word `service` when no tool or timer is responsible",
            "`3 of 3` is followed by the stop line",
            "A saved message that replay runs and rejects, because its handler failed or its result "
            "failed the checksum, names its tool or timer.",
            "A record refused before it runs, because it is malformed, out of sequence or from a "
            "different program, is `service`.",
            "`NAME` never holds a colon, and `REASON` is always one line.",
            "A store that will not open is E1409 with no `failure` line.",
            "A store another process holds, such as a second `mo serve` or a reader of `DIR`, is "
            "not E1409: `mo serve` waits up to 10 seconds for it",
            "A store failure while the service runs is a `stop` line with no `failure` line.",
            "The lines have no timestamps.",
            "`^mo serve: failure:`",
            "`journalctl -o cat`",
            "These line shapes are stable, and changing one is a breaking change.",
            "The words of `REASON` are not stable",
            "The token never appears in output, logs or errors.",
            "- 0: SIGTERM or SIGINT stopped it. It closes its port, so a new connection is refused, "
            "and finishes the open ones first, which their deadlines bound to about 25 seconds.",
            "- 69: supervision (D308) stopped the service.",
            "It stays down until a person starts it, as Recover from a stop says.",
            "any source-byte change, including comments, needs an explicit upgrade.",
            "`service.sqlite` is readable only by its owner.",
            "- 70: it died any other way",
            "- 75: another process held the store for the whole wait.",
            "Moving the same files does not change their source identity."):
        assert sentence in guide, f"docs/agent-guide.md no longer says: {sentence}"


def serve_command(binary):
    serve_guide()
    token = "McpServeCanary42"
    with tempfile.TemporaryDirectory(prefix="mo-serve-") as directory:
        folder = pathlib.Path(directory)
        data = folder / "state"
        data.mkdir()
        counter = "counter.mo"
        (folder / counter).write_text(SERVE_HALTING)
        env = {**os.environ, "COUNTER_TOKEN": token}
        outputs = []

        @contextlib.contextmanager
        def serving(source=counter, folder_for_data=data, port="0", environment=env, extra=()):
            served = Served(binary, source, folder_for_data, environment, folder, port, extra)
            try:
                yield served
            finally:
                served.close()
                outputs.append(served.line + served.errors())

        def refused(source=counter, folder_for_data=data, port="0", environment=env, extra=(),
                    code="E1409"):
            with serving(source, folder_for_data, port, environment, extra) as served:
                assert not served.line, served.line
                errors = served.finish(1)
                assert f"error {code}:".encode() in errors, errors
                return errors

        def source_refused(source, folder_for_data=data):
            database = folder_for_data / "service.sqlite"
            before = database.read_bytes()
            with sqlite3.connect(database) as db:
                digest = db.execute("SELECT source FROM program").fetchone()[0].hex()
            errors = refused(source, folder_for_data, code="E1430")
            assert errors == (f"error E1430: source differs from recorded digest {digest}; "
                              "use --upgrade\n").encode(), errors
            assert database.read_bytes() == before, "source refusal changed persistent state"
            assert serve_events(errors) == [], errors
            return errors

        with serving() as served:
            status, body = served.post(SERVE_INITIALIZE, token)
            assert status == 200 and json.loads(body)["result"]["protocolVersion"] == "2025-11-25"
            assert served.post(SERVE_INITIALIZED, token)[0] == 202
            status, body = served.post(SERVE_LIST, token)
            tools = json.loads(body)["result"]["tools"]
            assert status == 200 and [tool["name"] for tool in tools] == ["add", "read", "halt"] + SERVE_OPERATOR_NAMES + SERVE_UPGRADE_NAMES + SERVE_REVIEW_NAMES
            assert all(tool["inputSchema"]["type"] == "object" for tool in tools), tools
            assert serve_structured(served.post(SERVE_READ, token)) == {"value": "0"}
            assert serve_structured(served.post(SERVE_ADD, token)) == {"value": "5"}
            assert serve_structured(served.post(SERVE_READ, token)) == {"value": "5"}
            status, body = served.post(SERVE_READ, "WrongCanary42")
            assert status == 401 and token.encode() not in body, (status, body)
            assert served.post(SERVE_READ, token, {"Host": "elsewhere.example"})[0] == 421
            named = {"Host": f"localhost:{served.port}"}
            assert serve_structured(served.post(SERVE_READ, token, named)) == {"value": "5"}
            assert served.finish(0, signal.SIGTERM) == SERVE_SIGNALLED
        assert (data / "service.sqlite").is_file()
        for path in data.iterdir():
            assert stat.S_IMODE(path.stat().st_mode) == 0o600, path

        spare = folder / "spare"
        spare.mkdir()
        with serving(counter, spare) as served:
            assert served.finish(0, signal.SIGTERM) == SERVE_SIGNALLED
        shutil.rmtree(spare)
        spare.mkdir()

        proxied = folder / "proxied"
        proxied.mkdir()
        with serving(counter, proxied, extra=["--allow-host", "queue.example"]) as served:
            added = served.post(SERVE_ADD, token, {"Host": "queue.example"})
            assert serve_structured(added) == {"value": "5"}
            for host in ("other.example", "queue.example:443"):
                assert served.post(SERVE_READ, token, {"Host": host})[0] == 421, host
            for host in (f"127.0.0.1:{served.port}", f"localhost:{served.port}"):
                read = served.post(SERVE_READ, token, {"Host": host})
                assert serve_structured(read) == {"value": "5"}, host
            assert served.finish(0, signal.SIGTERM) == SERVE_SIGNALLED
        with serving(counter, proxied) as served:
            assert served.post(SERVE_READ, token, {"Host": "queue.example"})[0] == 421
            assert serve_structured(served.post(SERVE_ADD, token)) == {"value": "10"}
            assert served.finish(0, signal.SIGTERM) == SERVE_SIGNALLED
        with serving(counter, proxied, extra=["--allow-host", "queue.example:8000"]) as served:
            read = served.post(SERVE_READ, token, {"Host": "queue.example:8000"})
            assert serve_structured(read) == {"value": "10"}
            assert served.post(SERVE_READ, token, {"Host": "queue.example"})[0] == 421
            assert served.finish(0, signal.SIGTERM) == SERVE_SIGNALLED
        errors = refused(counter, proxied, extra=["--allow-host", "bad host"])
        assert errors.startswith(b"error E1409: mo serve could not start the service: "
                                 b"the server configuration was refused"), errors

        with serving() as served:
            body = SERVE_ADD.encode()
            connection = socket.create_connection(("127.0.0.1", served.port), timeout=30)
            connection.sendall(
                b"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nAuthorization: Bearer %s\r\n"
                b"Content-Type: application/json\r\nMCP-Protocol-Version: 2025-11-25\r\n"
                b"Expect: 100-continue\r\nContent-Length: %d\r\n\r\n"
                % (served.port, token.encode(), len(body)))
            interim = b""
            while not interim.endswith(b"\r\n\r\n"):
                interim += connection.recv(1)
            assert interim.startswith(b"HTTP/1.1 100 "), interim
            served.process.send_signal(signal.SIGTERM)
            time.sleep(0.4)
            assert served.process.poll() is None, "mo serve left before its open client finished"
            started = time.monotonic()
            with contextlib.suppress(ConnectionRefusedError):
                socket.create_connection(("127.0.0.1", served.port), timeout=30).close()
                raise AssertionError("a stopping mo serve accepted a connection")
            assert time.monotonic() - started < 1, "a stopping mo serve left a client waiting"
            connection.sendall(body)
            reply = b""
            while chunk := connection.recv(65536):
                reply += chunk
            connection.close()
            assert reply.startswith(b"HTTP/1.1 200 ") and b'"value":"10"' in reply, reply
            assert served.finish(0) == SERVE_SIGNALLED

        with serving() as served:
            assert serve_structured(served.post(SERVE_READ, token)) == {"value": "10"}
            saved = (data / "service.sqlite").read_bytes()
            started = time.monotonic()
            with serving() as second:
                assert not second.line, second.line
                errors = second.finish(75)
            waited = time.monotonic() - started
            assert 9.5 <= waited < 25, waited
            expected = b"mo serve: could not open saved state at %s/service.sqlite: %s\n" % (
                str(data).encode(), STORE_BUSY)
            assert errors == expected, errors
            assert (data / "service.sqlite").read_bytes() == saved
            blocked = socket.socket()
            blocked.bind(("127.0.0.1", 0))
            blocked.listen(1)
            taken = str(blocked.getsockname()[1])
            errors = refused(counter, spare, taken)
            assert b"could not listen on port " + taken.encode() + b": Address already in use" in errors
            blocked.close()
            assert served.finish(0, signal.SIGINT) == SERVE_SIGNALLED

        for spelling in serve_spellings(folder, counter):
            with serving(spelling) as served:
                assert serve_structured(served.post(SERVE_READ, token)) == {"value": "10"}, spelling
                assert served.finish(0, signal.SIGTERM) == SERVE_SIGNALLED
        edited = folder / "edited"
        edited.mkdir()
        for before, after in SERVE_EDITS:
            assert before in SERVE_HALTING, before
            (edited / counter).write_text(SERVE_HALTING.replace(before, after))
            source_refused(edited / counter)
        elsewhere = folder / "elsewhere"
        elsewhere.mkdir()
        (elsewhere / counter).write_text(SERVE_HALTING)
        served = Served(binary, counter, data, env, elsewhere)
        try:
            assert serve_structured(served.post(SERVE_READ, token)) == {"value": "10"}
            assert served.finish(0, signal.SIGTERM) == SERVE_SIGNALLED
        finally:
            served.close()
            outputs.append(served.line + served.errors())

        tree = folder / "tree"
        (tree / "shop").mkdir(parents=True)
        (tree / "shop" / "account.mo").write_text(
            "module Shop.Account\nexpose show\n\nfn show(value: Int) : Text\n"
            "  value.to_text\nend\n")
        (tree / "app.mo").write_text(
            SERVE_COUNTER.replace("module Counter", "module App\nuse Shop.Account")
            .replace("state.to_text", "Account.show(state)")
            .replace("## Reads the count.", "# note one\n## Reads the count."))
        modules = folder / "modules-state"
        modules.mkdir()
        served = Served(binary, "app.mo", modules, env, tree)
        try:
            assert serve_structured(served.post(SERVE_ADD, token)) == {"value": "5"}
            assert served.finish(0, signal.SIGTERM) == SERVE_SIGNALLED
        finally:
            served.close()
            outputs.append(served.line + served.errors())
        with serving(tree / "app.mo", modules) as served:
            assert serve_structured(served.post(SERVE_READ, token)) == {"value": "5"}
            assert served.finish(0, signal.SIGTERM) == SERVE_SIGNALLED
        (tree / "app.mo").write_text((tree / "app.mo").read_text().replace("# note one", "# note two"))
        source_refused(tree / "app.mo", modules)

        pages = folder / "pages"
        pages.mkdir()
        names = [f"t{i}" for i in range(SERVE_TOOL_PAGE + 1)]
        (pages / "paged.mo").write_text(
            'module Paged\n\nservice\n  state Int = 0\n  tools ' + ", ".join(names) +
            '\n  token from env "COUNTER_TOKEN"\nend\n' + "".join(
                f"\n## Reads the count.\nfn {name}(state: Int, _now: Time) : Text\n"
                "  state.to_text\nend\n" for name in names))
        paged_state = folder / "paged-state"
        paged_state.mkdir()
        served = Served(binary, "paged.mo", paged_state, env, pages)
        try:
            status, body = served.post(SERVE_LIST, token)
            assert status == 200, body
            cursor = json.loads(body)["result"]["nextCursor"]
            assert served.finish(0, signal.SIGTERM) == SERVE_SIGNALLED
        finally:
            served.close()
            outputs.append(served.line + served.errors())
        with serving(pages / "paged.mo", paged_state) as served:
            status, body = served.post(json.dumps(
                {"jsonrpc": "2.0", "id": 6, "method": "tools/list", "params": {"cursor": cursor}}),
                token)
            assert status == 200, body
            assert [tool["name"] for tool in json.loads(body)["result"]["tools"]] == names[-1:] + SERVE_OPERATOR_NAMES + SERVE_UPGRADE_NAMES + SERVE_REVIEW_NAMES, body
            assert served.finish(0, signal.SIGTERM) == SERVE_SIGNALLED

        with serving() as served:
            for attempt in range(15):
                status, body = served.post(SERVE_HALT, token)
                assert status == 200, (attempt, status, body)
                error = json.loads(body)["result"]["structuredContent"]["error"]
                assert error["code"] == "handler_failed", (attempt, body)
            errors = served.finish(69)
        assert not errors.endswith(SERVE_SIGNALLED), errors
        assert serve_events(errors) == serve_supervised(b"halt"), errors
        assert errors.count(b"halted on purpose") == 15, errors
        reported = b"mo serve: failure: halt: service handler failed\nbug: stop\n"
        assert errors.count(reported) == 15, errors

        fixed = folder / "fixed.mo"
        fixed.write_text(SERVE_HALTING.replace("module Counter", "module Fixed")
                         .replace('stop "halted on purpose"', '"fine"'))
        source_refused(fixed)
        renewed = folder / "renewed"
        renewed.mkdir()
        with serving(fixed, renewed) as served:
            assert serve_structured(served.post(SERVE_READ, token)) == {"value": "0"}
            assert serve_structured(served.post(SERVE_HALT, token)) == {"value": "fine"}
            assert served.finish(0, signal.SIGTERM) == SERVE_SIGNALLED

        with serving() as served:
            assert serve_structured(served.post(SERVE_READ, token)) == {"value": "10"}
            shutil.rmtree(data)
            error = serve_structured(served.post(SERVE_ADD, token))["error"]
            assert (error["code"], error["effect"]) == ("store_failed", "unknown"), error
            errors = served.finish(70)
        assert serve_events(errors) == [b"mo serve: stop: " + STORE_FAILED], errors

        errors = refused(counter, data)
        assert b"can't use the data folder" in errors and b"state: No such file" in errors, errors
        assert not data.exists(), "mo serve must not create its data folder"
        data.mkdir()
        errors = refused(counter, counter)
        assert b"Not a directory" in errors, errors

        missing = {key: value for key, value in env.items() if key != "COUNTER_TOKEN"}
        errors = refused(environment=missing)
        assert b"environment variable COUNTER_TOKEN" in errors, errors
        invalid = "Not a valid canary"
        errors = refused(environment={**env, "COUNTER_TOKEN": invalid})
        assert b"environment variable COUNTER_TOKEN" in errors, errors
        assert invalid.encode() not in errors, errors

        crash = folder / "crash.mo"
        crash.write_text(SERVE_COUNTER.replace("module Counter", "module Crash")
                         .replace("state Int = 0", 'state Int = stop "start refused"'))
        fresh = folder / "fresh"
        fresh.mkdir()
        errors = refused(crash, fresh)
        assert serve_events(errors) == [b"mo serve: failure: service: service handler failed"], errors
        assert b"start refused" in errors, errors
        assert (b"error E1409: mo serve could not start the service: "
                b"service handler failed") in errors, errors

        constant = folder / "constant.mo"
        constant.write_text(SERVE_COUNTER.replace("module Counter", "module Constant")
                            .replace("service\n", 'limit = (999.to_byte or stop "constant failed").to_int\n\nservice\n', 1)
                            .replace("  state.to_text\n", "  (state + limit).to_text\n"))
        served = Served(binary, constant, fresh, env, folder)
        try:
            assert not served.line, served.line
            errors = served.finish(70)
            assert b"constant failed" in errors and b"E1409" not in errors, errors
        finally:
            served.close()
            outputs.append(served.line + served.errors())

        with open("/dev/full", "wb") as full:
            result = subprocess.run([str(binary), "serve", counter, "--port", "0", "--data",
                                     str(fresh)], env=env, stdout=full, stderr=subprocess.PIPE,
                                    cwd=folder, timeout=30)
        assert result.returncode == 1 and b"E1409" in result.stderr, result
        assert b"could not write its listening line" in result.stderr, result

        (folder / "aaa.mo").write_text('module Aaa\nexpose peek\n\nservice\n  state Int = 7\n'
                                       '  tools peek\n  token from env "MO_SERVE_OTHER_TOKEN"\n'
                                       'end\n\n## Peeks.\nfn peek(state: Int, _now: Time) : Text\n'
                                       '  state.to_text\nend\n')
        entry = folder / "zzz.mo"
        entry.write_text('module Zzz\nuse Aaa\n\nservice\n  state Int = 3\n  tools look\n'
                         '  token from env "COUNTER_TOKEN"\nend\n\n## Looks.\n'
                         'fn look(state: Int, now: Time) : Text\n'
                         '  "#{state}#{Aaa.peek(1, now)}"\nend\n')
        imported = folder / "imported"
        imported.mkdir()
        with serving(entry, imported) as served:
            status, body = served.post(SERVE_LIST, token)
            tools = json.loads(body)["result"]["tools"]
            assert status == 200 and [tool["name"] for tool in tools] == ["look"] + SERVE_OPERATOR_NAMES + SERVE_UPGRADE_NAMES + SERVE_REVIEW_NAMES, body
            look = SERVE_READ.replace('"read"', '"look"')
            assert serve_structured(served.post(look, token)) == {"value": "31"}
            assert served.finish(0, signal.SIGTERM) == SERVE_SIGNALLED
        def timed(name, every, parameter, body):
            source = (SERVE_COUNTER.replace("module Counter", f"module {name}")
                      .replace("  token from", f"  every {every} tick\n  token from"))
            path = folder / f"{name.lower()}.mo"
            path.write_text(source + f"\nfn tick({parameter}: Int, _now: Time) : Int\n  {body}\nend\n")
            state = folder / f"{name.lower()}-state"
            state.mkdir()
            return path, state

        path, state = timed("Ticking", "1.hour", "state", "state + 1")
        with serving(path, state) as served:
            assert serve_structured(served.post(SERVE_READ, token)) == {"value": "1"}
            assert served.finish(0, signal.SIGTERM) == SERVE_SIGNALLED
        path.write_text(path.read_text().replace("## Reads the count.", "## Reads the total."))
        source_refused(path, state)
        path, state = timed("Failing", "1.millisecond", "_state", 'stop "tick failed"')
        with serving(path, state) as served:
            errors = served.finish(69)
        assert serve_events(errors) == serve_supervised(b"tick"), errors
        forging = folder / "forging.mo"
        forging.write_text(SERVE_FORGING)
        state = folder / "forging-state"
        state.mkdir()
        with serving(forging, state) as served:
            errors = served.finish(69)
        assert serve_events(errors) == serve_supervised(b"tick"), errors
        assert errors.count(SERVE_FORGED.encode()) == 30, errors

        path, state = timed("Overflowing", "9223372036854775807.hours", "state", "state + 1")
        errors = refused(path, state)
        assert serve_events(errors) == [b"mo serve: failure: tick: service handler failed"], errors
        assert b"bug: overflow: Duration" in errors, errors

        overloaded = folder / "overloaded.mo"
        overloaded.write_text(SERVE_OVERLOADED)
        state = folder / "overloaded-state"
        state.mkdir()
        with serving(overloaded, state) as served:
            assert serve_structured(served.post(SERVE_HALT, token))["error"]["code"] == "handler_failed"
            errors = served.finish(0, signal.SIGTERM)
        assert serve_events(errors) == [b"mo serve: failure: tick: service handler failed",
                                        b"mo serve: failure: halt: service handler failed",
                                        SERVE_SIGNALLED.rstrip()], errors

        plain = folder / "plain.mo"
        plain.write_text("module Plain\nuse Aaa\n\nfn main(console: Console)\n"
                         "  now = Time.parse(\"2026-01-01T00:00:00Z\") or stop \"valid time\"\n"
                         "  console.print(Aaa.peek(1, now))\nend\n")
        plain_state = folder / "plain-state"
        plain_state.mkdir()
        errors = refused(plain, plain_state)
        assert b"this module has no service to start" in errors, errors

        for output in outputs:
            assert token.encode() not in output, output
        for path in folder.rglob("*"):
            if path.is_file():
                assert token.encode() not in path.read_bytes(), path
