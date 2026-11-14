import pathlib
import shutil
import tempfile

from cli_common import records, root, run


def promise_selection(binary):
    opaque = 'expose Box\nstruct Box\nvalue: Int\nend\n'
    cases = [
        ('fn(Int) : Int', 'identity', 'fn identity(value: Int) : Int\nvalue\nend\n'),
        ('List(fn(Int) : Int)', '[identity]', 'fn identity(value: Int) : Int\nvalue\nend\n'),
        ('Box', 'Box(value: 3)', opaque),
        ('List(Box)', '[]', opaque),
        ('Option(Box)', 'None', opaque),
        ('Result(Int, Box)', 'Ok(13)', opaque),
        ('Choice', 'Open', opaque + 'enum Choice\nOpen\nClosed(Box)\nend\n'),
        ('(Text, Box)', '("owned", Box(value: 3))', opaque),
    ]
    # Shapes can start a Box with its exposed make, so these are built through it (reference 12.2).
    buildable = [
        ('Box', 'make(3)', 'use Shapes{Box, make}\n'),
        ('List(Shapes.Box)', '[]', 'use Shapes\n'),
        ('Shapes.Box', 'Shapes.make(3)',
         'expose Box(..)\nuse Shapes\nstruct Box\nother: Text\nend\n'),
    ]
    with tempfile.TemporaryDirectory(prefix='mo-promise-selection-') as directory:
        path = pathlib.Path(directory) / 'main.mo'
        (path.parent / 'shapes.mo').write_text(
            'module Shapes\nexpose Box, make\nstruct Box\nvalue: Int\nend\n'
            'fn make(value: Int) : Box\nBox(value: value)\nend\n')
        for type_, input_, declarations in cases + buildable:
            built = (type_, input_, declarations) in buildable
            initializer = 'seed = crash()\nfn crash() : Int\nstop "must not initialize"\nend\n'
            source = ('module Main\n' + declarations + initializer +
                            'test "first observation"\nassert true\nend\n'
                            'fn supported(value: List(Text)) : List(Text)\n'
                            'promise "supported first"\nresult == value\n'
                            'allows value: ["owned"], result: ["owned"]\n'
                            'forbids value: [], result: ["é"]\nend\nvalue\nend\n'
                            f'fn probe(_value: {type_}) : Int\npromise "seven"\nresult == 7\n'
                            f'allows _value: {input_}, result: 7\nforbids _value: {input_}, result: 8\nend\n'
                            '7\nend\nfn main()\nstop "main ran"\nend\n')
            path.write_text(source)
            items = records(run(binary, ['test', '--json', '--list', '--max-heap', '1', str(path)], 0), 'test')
            assert [r['name'] for r in items[1:-1]] == [
                'Main.test.1', 'Main.supported.promise.1', 'Main.probe.promise.1']
            items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
            assert len(items) == 3 and items[1]['code'] == ('E1406' if built else 'E1400')
            if not built:
                assert items[1]['primary']['label'] == 'Main.probe.promise.1'
            assert items[-1]['passed'] == items[-1]['failed'] == items[-1]['stopped'] == 0
            # Selection, not the mere presence of a deferred promise, controls
            # preflight. Initialization is reached for ordinary-only selection.
            items = records(run(binary, ['test', '--json', '--only', 'Main.test.1', str(path)], 1), 'test')
            assert items[1]['code'] == 'E1406' and items[-1]['passed'] == 0
            path.write_text(source.replace(initializer, ''))
            items = records(run(binary, ['test', '--json', '--only', 'Main.test.1', str(path)], 0), 'test')
            assert items[1]['outcome'] == 'passed' and items[-1]['passed'] == 1
            if built:
                items = records(run(binary, ['test', '--json', str(path)], 0), 'test')
                assert [r['outcome'] for r in items[1:-1]] == ['passed'] * 3

        # Same-module written access to exported opaque fields remains legal;
        # only structural generation is refused. A private local type is not
        # accidentally classified as exported opaque.
        source = ('module Main\nexpose Box\nstruct Box\nvalue: Int\nend\n'
                  'fn probe(value: Box) : Int\npromise "visible only to source"\nresult == value.value\n'
                  'allows value: Box(value: 13), result: 13\n'
                  'forbids value: Box(value: 13), result: 14\nend\nvalue.value\nend\n'
                  'test "own fields"\nassert Box(value: 13).value == 13\nend\nfn main()\nend\n')
        path.write_text(source)
        records(run(binary, ['check', '--json', str(path)], 0), 'check')
        items = records(run(binary, ['test', '--json', '--only', 'Main.test.1', str(path)], 0), 'test')
        assert items[-1]['passed'] == 1
        items = records(run(binary, ['test', '--json', str(path)], 1), 'test')
        assert items[1]['code'] == 'E1400' and items[-1]['passed'] == 0
        path.write_text(source.replace('expose Box\n', ''))
        items = records(run(binary, ['test', '--json', str(path)], 0), 'test')
        assert items[-1]['passed'] == 2 and items[-1]['errors'] == 0


COUNTER = '''module Main

service
  state Int = 0
  tools add, total
  token from env "COUNTER_TOKEN"
end

struct Total
  count: Int
end

fn add(state: Int, _now: Time, amount: Int) : (Int, Unit)
  (state + amount, Unit)
end

fn total(state: Int, _now: Time) : Total
  Total(count: state)
end
'''


def recovery_checks(binary):
    """Every service gets one recovery check, listed, selected, run and reported like the others
    (reference 12.5 and 13.4). The failure report is tested in tests/test_recovery.c, and a
    deadline that passed before the plan began in tests/test_deadline.c."""
    queue = "services/task-queue/main.mo"
    listed = run(binary, ["test", "--list", queue], 0).stdout.decode().splitlines()
    assert listed[-1] == "Main.service.recovery" and "JobQueue.test.1" in listed, listed
    assert len(listed) == len(set(listed)), listed
    items = records(run(binary, ["test", "--list", "--json", "--in", "Main", queue], 0), "test")
    assert [(r["name"], r["kind"]) for r in items[1:-1]] == [
        *((f"Main.submit.example.{n}", "example") for n in range(1, 4)),
        ("Main.list.promise.1", "promise"),
        *((f"Main.list.example.{n}", "example") for n in range(1, 4)),
        *((f"Main.expire.example.{n}", "example") for n in range(1, 3)),
        *((f"Main.service.promise.{n}", "promise") for n in range(1, 11)),
        ("Main.service.recovery", "recovery")], items
    span = items[-2]["span"]
    assert (span["line"], span["column"], span["end_line"]) == (15, 1, 149), span
    tests = "services/task-queue/queue-tests.mo"
    listed = run(binary, ["test", "--list", "--in", "QueueTests", tests], 0).stdout.decode()
    assert "QueueTests.test.1\n" in listed and "service" not in listed, listed
    listed = run(binary, ["test", "--list", "--in", "Main", tests], 0).stdout.decode()
    assert listed.endswith("Main.service.promise.10\nMain.service.recovery\n"), listed
    # --only runs the one check; --calls caps its sequences, and JSON carries the counts.
    items = records(run(binary, ["test", "--json", "--calls", "3", "--only",
                                 "Main.service.recovery", queue], 0), "test")
    assert len(items) == 3 and items[1]["kind"] == "recovery", items
    result = items[1]
    assert result["outcome"] == "passed" and result["sequences"] == 3, result
    assert type(result["restarts"]) is int and result["restarts"] >= 3, result
    assert "steps" not in result and "before" not in result, result
    assert items[-1]["passed"] == 1 and items[-1]["status"] == 0, items

    with tempfile.TemporaryDirectory(prefix="mo-recovery-") as directory:
        path = pathlib.Path(directory) / "main.mo"
        # A service and nothing else is a program with one check to run, not a command mistake.
        path.write_text(COUNTER)
        expected = f"passed Main.service.recovery  {path}:3:1\n1 passed, 0 failed, 0 stopped\n"
        first, second = (run(binary, ["test", str(path)], 0) for _ in range(2))
        assert first.stdout == second.stdout == expected.encode() and not first.stderr, first
        assert run(binary, ["test", "--list", str(path)], 0).stdout == b"Main.service.recovery\n"
        items = records(run(binary, ["test", "--json", "--calls", "7", str(path)], 0), "test")
        assert items[1]["sequences"] == 7 and items[1]["restarts"] >= 7, items
        for selectors, message in (
                (["--only", "Main.service.recover"],
                 "--only does not name an example, promise, test or recovery check"),
                (["--in", "Other"],
                 "test selection matches no examples, promises, tests or recovery checks")):
            result = run(binary, ["test", *selectors, str(path)], 2)
            assert not result.stdout and result.stderr == f"{message}\n".encode(), result

        # Beside a test, the check comes last; the test's own result keeps its shape.
        path.write_text(COUNTER + 'test "adds"\n  now = Time.parse("2026-01-01T00:00:00Z") or stop "time"\n'
                        '  assert add(1, now, 2) == (3, Unit)\nend\n')
        items = records(run(binary, ["test", "--json", "--calls", "2", str(path)], 0), "test")
        assert [(r["name"], r["kind"]) for r in items[1:-1]] == [
            ("Main.test.1", "test"), ("Main.service.recovery", "recovery")], items
        assert set(items[1]) == {"record", "name", "kind", "span", "outcome", "microseconds"}

        # A start value that stops is the service's own failure: the check stops with it.
        path.write_text(COUNTER.replace("state Int = 0", 'state Int = stop "no start"'))
        result = run(binary, ["test", str(path)], 1)
        assert result.stdout == (f"stopped Main.service.recovery  {path}:3:1\n"
                                 "  bug: stop\n  detail: no start\n"
                                 f"  in the start value  {path}:4:15\n"
                                 "0 passed, 0 failed, 1 stopped\n").encode(), result
        items = records(run(binary, ["test", "--json", str(path)], 1), "test")
        defect = items[1]["defect"]
        assert items[1]["outcome"] == "stopped" and items[1]["restarts"] == 0, items
        assert (defect["reason"], defect["detail"], defect["span"]["line"]) == ("stop", "no start", 4)


SERVICE_PROMISES = COUNTER.replace('  token from env "COUNTER_TOKEN"\nend\n', '''  token from env "COUNTER_TOKEN"

  promise "reading the total changes nothing" on total
    after == before and reply.count == before
    allows before: 1, reply: Total(count: 1), after: 1, now: t0
    forbids before: 1, reply: Total(count: 1), after: 2, now: t0
  end

  promise "an addition adds its amount" on add
    after == before + amount
    allows before: 1, amount: 2, reply: Unit, after: 3, now: t0
    forbids before: 1, amount: 2, reply: Unit, after: 1, now: t0
  end
end
''') + 't0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"\n'


def without_time(result):
    """A run's JSON records without the one field that measures the run, not the program."""
    return [{k: v for k, v in r.items() if k != "microseconds"} for r in records(result, "test")]


def service_promise_checks(binary):
    """Service promises are listed before their service's recovery check, run its whole plan
    whatever --seconds says, and report the same records on every run (reference 12.2, 12.5)."""
    with tempfile.TemporaryDirectory(prefix="mo-service-promises-") as directory:
        path = pathlib.Path(directory) / "main.mo"
        path.write_text(SERVICE_PROMISES)
        listed = run(binary, ["test", "--list", str(path)], 0).stdout
        assert listed == (b"Main.service.promise.1\nMain.service.promise.2\n"
                          b"Main.service.recovery\n"), listed
        items = without_time(run(binary, ["test", "--json", str(path)], 0))
        assert [(r["name"], r["kind"], r["outcome"]) for r in items[1:-1]] == [
            ("Main.service.promise.1", "promise", "passed"),
            ("Main.service.promise.2", "promise", "passed"),
            ("Main.service.recovery", "recovery", "passed")], items
        assert all(r["sequences"] == 200 for r in items[1:-1]), items
        assert "restarts" not in items[1] and items[3]["restarts"] >= 200, items
        assert without_time(run(binary, ["test", "--json", "--seconds", "1", str(path)], 0)) == items
        only = records(run(binary, ["test", "--json", "--only", "Main.service.promise.2", str(path)],
                           0), "test")
        assert [r["name"] for r in only[1:-1]] == ["Main.service.promise.2"], only
    failing = "tests/golden/service-promise-restart/main.mo"
    first, second = (without_time(run(binary, ["test", "--json", failing], 1)) for _ in range(2))
    assert first == second and first[1]["steps"][-1]["broke"] == "Main.service.promise.1", first


STRENGTH = "tests/cli/strength"
STRENGTH_OUTCOMES = {
    "a-negative.mo": "killed_by_promise", "b-nine.mo": "killed_by_example",
    "c-no-ceiling.mo": "killed_by_both", "d-seven.mo": "survived",
    "e-limit.mo": "changes_intent", "f-text.mo": "invalid", "g-stop.mo": "defect",
    "h-unknown.mo": "invalid"}


def strength_record(binary, program, versions, status=0):
    items = without_time(run(binary, ["test", "--json", program, "--strength", versions], status))
    assert items[-2]["record"] == "strength" and items[-2]["set"] == versions, items
    return items[-2]


def strength_checks(binary):
    versions = f"{STRENGTH}/versions"
    record = strength_record(binary, f"{STRENGTH}/main.mo", versions)
    assert {v["name"]: v["outcome"] for v in record["versions"]} == STRENGTH_OUTCOMES, record
    assert (record["killed"], record["scored"], record["rate"], record["partial"],
            record["counted_file"], record["original"]) == (3, 4, 0.75, False, False, "passed")
    assert [v.get("reason") for v in record["versions"] if v["outcome"] == "invalid"] == [
        "E0078: a function's result doesn't match its signature",
        "a declaration it replaces is missing, overloaded or replaced twice"], record
    assert strength_record(binary, f"{STRENGTH}/main.mo", versions) == record
    text = run(binary, ["test", f"{STRENGTH}/main.mo", "--strength", versions], 0).stdout
    assert (b"strength: 3 of 4 wrong versions killed (75.0%); 2 invalid, 1 changes_intent, "
            b"1 defect, 0 unfinished, 0 not_counted\n") in text, text
    source = (root / STRENGTH / "main.mo").read_text()
    with tempfile.TemporaryDirectory(prefix="mo-strength-") as directory:
        folder = pathlib.Path(directory)
        shutil.copytree(root / versions, folder / "versions")
        stronger = folder / "main.mo"
        stronger.write_text(source.replace(
            "  examples\n",
            "  promise \"the result is the ceiling of a natural input\"\n"
            "    n < 0 or result == ceiling(n)\n"
            "    allows n: 3, result: 3\n    forbids n: 3, result: 4\n  end\n\n  examples\n", 1))
        unlisted = strength_record(binary, str(stronger), str(folder / "versions"))
        assert (unlisted["killed"], unlisted["scored"], unlisted["rate"]) == (2, 2, 1.0), unlisted
        (folder / "versions" / "counted").write_text(
            "a-negative.mo\nb-nine.mo\nc-no-ceiling.mo\nd-seven.mo\n")
        counted = strength_record(binary, str(stronger), str(folder / "versions"))
        assert {v["name"]: v["outcome"] for v in counted["versions"]} == {
            **{name: "not_counted" for name in STRENGTH_OUTCOMES},
            "a-negative.mo": "killed_by_promise", "b-nine.mo": "killed_by_both",
            "c-no-ceiling.mo": "changes_intent", "d-seven.mo": "changes_intent"}, counted
        assert (counted["killed"], counted["scored"], counted["counted_file"]) == (2, 4, True)
        (folder / "control").mkdir()
        control = folder / "control" / "main.mo"
        control.write_text(source[:source.index("  promise")] +
                           "  promise \"the result is never one particular number\"\n"
                           "    result != 123456789\n"
                           "    allows n: 3, result: 3\n    forbids n: 3, result: 123456789\n"
                           "  end\n\n" + source[source.index("  return 0"):])
        (folder / "versions" / "counted").unlink()
        weak = strength_record(binary, str(control), str(folder / "versions"))
        assert (weak["killed"], weak["scored"], weak["rate"]) == (0, 6, 0.0), weak
        (folder / "broken").mkdir()
        broken = folder / "broken" / "main.mo"
        broken.write_text(source.replace("  return 0 if n < 0\n", ""))
        failed = strength_record(binary, str(broken), str(folder / "versions"), 1)
        assert (failed["original"], failed["versions"], failed["rate"]) == ("failed", [], None)
        (folder / "versions" / "counted").symlink_to("counted")
        unreadable = run(binary, ["test", str(stronger), "--strength", str(folder / "versions")], 2)
        assert b"the counted file could not be read" in unreadable.stderr, unreadable
    refused = run(binary, ["test", "--list", f"{STRENGTH}/main.mo", "--strength", versions], 2)
    assert b"--strength runs a fixed plan over the whole program" in refused.stderr, refused
    builder = without_time(run(binary, ["test", "--json", "tests/cli/strength-builder/main.mo",
                                        "--strength", "tests/cli/strength-builder/versions",
                                        "--seconds", "1"], 1))
    cut = next(r for r in builder if r["record"] == "strength")
    assert ([v["outcome"] for v in cut["versions"]], cut["partial"]) == (["unfinished"], True), cut
    library = without_time(run(binary, ["test", "--list", "--json", "lib/list.mo"], 0))
    assert all("digest" in r for r in library if r.get("kind") in ("promise", "example")), library


INTENT = "tests/cli/intent"
CLAMP_PROMISE = ('promise "{}"\n  within(result)\n  allows n: 3, result: 3\n'
                 '  forbids n: 20, result: 20\nend\n')


def intent_records(binary, args, status=0):
    items = records(run(binary, ["intent", "--json", *args], status), "intent")
    assert items[-2]["record"] == "intent", items
    return items[1:-2], items[-2]


def intent_command(binary):
    listed, end = intent_records(binary, [f"{INTENT}/before/main.mo"])
    assert end == {"record": "intent", "items": 5}, end
    assert [(r["record"], r["name"], r["kind"], r["owner"], r["text"], r["reads"]) for r in listed] == [
        ("item", "Main.within.example.1", "example", "Main.within", "within(3)\n", []),
        ("item", "Main.clamp.promise.1", "promise", "Main.clamp",
         CLAMP_PROMISE.format("the result stays within the limit"), ["Main.limit", "Main.within"]),
        ("item", "Main.clamp.example.1", "example", "Main.clamp", "clamp(3) == 3\n", []),
        ("item", "Main.clamp.example.2", "example", "Main.clamp", "clamp(20) == limit\n",
         ["Main.limit"]),
        ("item", "Main.clamp.example.3", "example", "Main.clamp", "clamp(-1) == 0\n", [])], listed
    tested = without_time(run(binary, ["test", "--list", "--json", f"{INTENT}/before/main.mo"], 0))
    assert [r["digest"] for r in tested[1:-1]] == [r["digest"] for r in listed], tested
    changes, end = intent_records(binary, [f"{INTENT}/after/main.mo", "--against",
                                           f"{INTENT}/before/main.mo"])
    assert end == {"record": "intent", "items": 5, "unchanged": 2, "edited": 1, "removed": 1,
                   "added": 1, "moved": 1}, end
    shown = [(r["change"], r.get("before", {}).get("name"), r.get("after", {}).get("name"))
             for r in changes]
    assert shown == [("moved", "Main.within.example.1", "Main.clamp.example.3"),
                     ("edited", "Main.clamp.promise.1", "Main.clamp.promise.1"),
                     ("removed", "Main.clamp.example.3", None),
                     ("added", None, "Main.within.promise.1")], changes
    moved, edited, removed, added = changes
    assert added["approved"] is False and "approved" not in edited, added
    assert [(r["name"], r["before"]) for r in moved["reads"]] == [
        ("Main.limit", None), ("Main.within", None)], moved
    assert moved["reads"][0]["after"] == "limit = 10\n", moved
    assert (edited["before"]["text"], edited["after"]["text"], edited["reads"]) == (
        CLAMP_PROMISE.format("the result stays within the limit"),
        CLAMP_PROMISE.format("the result never leaves the limit"), []), edited
    assert (removed["before"]["text"], "after" in removed) == ("clamp(-1) == 0\n", False), removed
    assert added["after"]["text"].startswith('promise "zero is always within"\n'), added
    text = run(binary, ["intent", f"{INTENT}/after/main.mo", "--against",
                        f"{INTENT}/before/main.mo"], 0).stdout.decode()
    for line in ("moved Main.within.example.1 to Main.clamp.example.3\n",
                 "edited Main.clamp.promise.1\n", '    promise "the result stays within the limit"\n',
                 '    promise "the result never leaves the limit"\n',
                 "removed Main.clamp.example.3\n", "added, unapproved Main.within.promise.1\n",
                 "intent: 2 unchanged, 1 edited, 1 removed, 1 added, 1 moved\n"):
        assert line in text, (line, text)
    constant, end = intent_records(binary, [f"{INTENT}/limit/main.mo", "--against",
                                            f"{INTENT}/before/main.mo"])
    assert (end["unchanged"], end["edited"]) == (3, 2), end
    assert [(r["change"], r["after"]["name"], r["reads"]) for r in constant] == [
        (change, name, [{"name": "Main.limit", "before": "limit = 10\n", "after": "limit = 12\n"}])
        for change, name in (("edited", "Main.clamp.promise.1"),
                             ("edited", "Main.clamp.example.2"))], constant
    text = run(binary, ["intent", f"{INTENT}/before/main.mo"], 0).stdout.decode()
    assert "examples Main.clamp\n" in text and "  reads Main.limit, Main.within\n" in text, text
    missing = run(binary, ["intent", f"{INTENT}/before/main.mo", "--against",
                           f"{INTENT}/missing/main.mo"], 1)
    assert b"the --against program does not check" in missing.stderr, missing
    assert "  now reads Main.limit\n    limit = 10\n" in run(binary, [
        "intent", f"{INTENT}/after/main.mo", "--against", f"{INTENT}/before/main.mo"], 0
    ).stdout.decode()
    assert "  no longer reads Main.limit\n    limit = 10\n" in run(binary, [
        "intent", f"{INTENT}/before/main.mo", "--against", f"{INTENT}/after/main.mo"], 0
    ).stdout.decode()
    rows, end = intent_records(binary, [f"{INTENT}/rows-before/main.mo"])
    assert rows[0]["reads"] == ["Main.P", "Main.Q", "Main.score@P", "Main.score@Q"], rows
    text = run(binary, ["intent", f"{INTENT}/rows-before/main.mo"], 0).stdout.decode()
    assert text.count("examples Main.double\n") == 1, text
    assert text.index("double(3) == 6") < text.index("promise Main.double.promise.1"), text
    changes, end = intent_records(binary, [f"{INTENT}/rows-after/main.mo", "--against",
                                           f"{INTENT}/rows-before/main.mo"])
    assert (end["unchanged"], end["edited"]) == (3, 2), end
    assert [(r["name"], r["before"].split("\n")[0], r["after"].split("\n")[0])
            for r in changes[0]["reads"]] == [
        ("Main.score@P", "fn score(p: P) : Int", "fn score(z: P) : Int")], changes[0]
    text = run(binary, ["intent", f"{INTENT}/rows-after/main.mo", "--against",
                        f"{INTENT}/rows-before/main.mo"], 0).stdout.decode()
    assert "edited Main.double.example.1 to Main.double.example.3\n" in text, text
    service, end = intent_records(binary, ["services/task-queue/main.mo"])
    promised = [r for r in service if r["kind"] == "service promise"]
    assert promised and all(r["owner"] == "Main.service" for r in promised), service
    assert promised[0]["name"] == "Main.service.promise.1", promised


def fix_command(binary):
    """`mo fix` applies every fix offered alone, checks again, and reports what is left (MO-48)."""
    with tempfile.TemporaryDirectory(prefix='mo-fix-') as directory:
        folder = pathlib.Path(directory)
        # The acceptance: after mo fix, mo check exits 0.
        alone = folder / 'alone'
        alone.mkdir()
        fixable = alone / 'main.mo'
        fixable.write_bytes((root / 'tests/cli/fix/main.mo').read_bytes())
        fixable.chmod(0o640)
        result = run(binary, ['fix', str(fixable)], 0)
        assert not result.stdout and not result.stderr, result
        assert fixable.read_text() == 'module Main\nfn run(_value: Int) : Int\n  1\nend\n'
        assert fixable.stat().st_mode & 0o777 == 0o640
        run(binary, ['check', str(fixable)], 0)
        result = run(binary, ['fix', str(fixable)], 0)
        assert not result.stdout and not result.stderr, result
        assert not list(alone.glob('.mo-write-*'))

        # E0025's fixes write or, and and not, spaced from their neighbours (D298): not gets a space
        # before it only after a word. An error stops its declaration, so the second and third in
        # pick land on later rounds.
        boolean = folder / 'boolean' / 'main.mo'
        boolean.parent.mkdir()
        boolean.write_text('module Main\n\nfn pick(a: Bool, b: Bool, c: Bool) : Bool\n'
                           '  (a&&b) || (!c)\nend\n\nfn flip(b: Bool) : Int\n  if!b\n    1\n'
                           '  else\n    2\n  end\nend\n')
        result = run(binary, ['fix', str(boolean)], 0)
        assert not result.stdout and not result.stderr, result
        assert boolean.read_text() == ('module Main\n\nfn pick(a: Bool, b: Bool, c: Bool) : Bool\n'
                                       '  (a and b) or (not c)\nend\n\nfn flip(b: Bool) : Int\n'
                                       '  if not b\n    1\n  else\n    2\n  end\nend\n')
        run(binary, ['check', str(boolean)], 0)

        # The removed repeated import uncovers the unnamed field, fixed on the next round. Only the
        # given file is rewritten; the imported module's fix is reported, and so is the unused
        # parameter, which has no fix while the module imports names.
        main = folder / 'main.mo'
        shapes = folder / 'shapes.mo'
        main.write_text('module Main\n\nuse Shapes { Shape, Shape }\n\n'
                        'fn area(shape: Shape, scale: Int) : Int\n  case shape\n'
                        '    Square(s): s * s\n  end\nend\n\nfn main()\nend\n')
        shapes_source = ('module Shapes\n\nexpose Shape(..)\n\nenum Shape\n  Square(side: Int)\n'
                         'end\n\nfn unused(count: Int) : Int\n  1\nend\n')
        shapes.write_text(shapes_source)
        result = run(binary, ['fix', str(main)], 1)
        assert not result.stdout
        assert result.stderr.count(b'error ') == 2, result.stderr
        assert b'main.mo:5:23' in result.stderr and b'shapes.mo:9:11' in result.stderr
        assert main.read_text() == ('module Main\n\nuse Shapes { Shape }\n\n'
                                    'fn area(shape: Shape, scale: Int) : Int\n  case shape\n'
                                    '    Square(side: s): s * s\n  end\nend\n\nfn main()\nend\n')
        assert shapes.read_text() == shapes_source
        # Given both files, each is checked as the program it starts and fixed. An error in a
        # given module is reported once, by its own file's check, not by every program using it.
        shapes.write_text(shapes_source + '\nfn bad() : Int\n  "x"\nend\n')
        result = run(binary, ['fix', str(main), str(shapes)], 1)
        assert result.stderr.count(b'error ') == 2, result.stderr
        assert b'main.mo:5:23' in result.stderr and result.stderr.count(b'shapes.mo:14:3') == 1
        assert 'fn unused(_count: Int)' in shapes.read_text()
        result = run(binary, ['fix', str(main)], 1)
        assert b'shapes.mo:14:3' in result.stderr

        # A fixed file that would outgrow the file size limit is left as it was.
        full = folder / 'full' / 'main.mo'
        full.parent.mkdir()
        head = b'module Main\nfn run(value: Int) : Int\n  1\nend\n'
        line = b'# ' + b'x' * 77 + b'\n'
        rest = 8 * 1024 * 1024 - len(head)
        body = line * (rest // len(line))
        full_source = head + body + b'#' * (rest - len(body) - 1) + b'\n'
        assert len(full_source) == 8 * 1024 * 1024
        full.write_bytes(full_source)
        result = run(binary, ['fix', str(full)], 1)
        assert b'E1407: the fixed file exceeds' in result.stderr and b'E0052' in result.stderr
        assert full.read_bytes() == full_source

        # A file fix can't replace is refused like format's, and the rest are still fixed.
        fixable.write_bytes((root / 'tests/cli/fix/main.mo').read_bytes())
        link = folder / 'link.mo'
        link.symlink_to(fixable)
        result = run(binary, ['fix', str(link), str(folder / 'missing.mo'), str(fixable)], 1)
        assert result.stderr.count(b'E1407') == 2 and result.stderr.count(b'error ') == 2
        assert link.is_symlink() and '_value' in fixable.read_text()
        assert not list(folder.glob('.mo-write-*'))
