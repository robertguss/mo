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
                assert items[-1]['errors'] == 0

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
        *((f"Main.current?.example.{n}", "example") for n in range(1, 3)),
        ("Main.matching.example.1", "example"),
        *((f"Main.service.promise.{n}", "promise") for n in range(1, 8)),
        ("Main.service.recovery", "recovery")], items
    span = items[-2]["span"]
    assert (span["line"], span["column"], span["end_line"]) == (15, 1, 102), span
    tests = "services/task-queue/queue-tests.mo"
    listed = run(binary, ["test", "--list", "--in", "QueueTests", tests], 0).stdout.decode()
    assert "QueueTests.test.1\n" in listed and "service" not in listed, listed
    listed = run(binary, ["test", "--list", "--in", "Main", tests], 0).stdout.decode()
    assert listed.endswith("Main.service.promise.7\nMain.service.recovery\n"), listed
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
        expected = (f"passed Main.service.recovery  {path}:3:1\n"
                    "1 passed, 0 failed, 0 stopped\n")
        first, second = (run(binary, ["test", str(path)], 0) for _ in range(2))
        assert first.stdout == second.stdout == expected.encode(), first
        assert not first.stderr and not second.stderr, first
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
        mutant_listed = run(binary, ["test", "--mutants", "--list", str(path)], 0).stdout
        assert mutant_listed == (b"Main.service.promise.1\nMain.service.promise.2\n"
                                 b"Main.service.recovery\nmutant " + str(path).encode() +
                                 b":26:10 Main.add: arithmetic, `+` to `-`\n"
                                 b"function Main.add: 1 mutant\nfunction Main.total: 0 mutants\n"), mutant_listed
        full = without_time(run(binary, ["test", "--json", str(path)], 0))
        items = full[1:-1]
        assert "mutants" not in full[-1] and "gates" not in full[-1], full
        assert [(r["name"], r["kind"], r["outcome"]) for r in items] == [
            ("Main.service.promise.1", "promise", "passed"),
            ("Main.service.promise.2", "promise", "passed"),
            ("Main.service.recovery", "recovery", "passed")], items
        mutated = without_time(run(binary, ["test", "--mutants", "--json", str(path)], 0))
        assert mutated[-1]["mutants"] == {"tried": 1, "killed": {
            "promise": 1, "example": 0, "test": 0, "recovery": 0, "start": 0},
            "survived": 0, "dropped": 0}, mutated
        assert all(r["sequences"] == 200 for r in items), items
        assert "restarts" not in items[0] and items[2]["restarts"] >= 200, items
        limited = without_time(run(binary, ["test", "--json", "--seconds", "1", str(path)], 0))
        assert limited == full, limited
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


def strength_record(binary, program, versions, status=0, extra=()):
    items = without_time(run(binary, ["test", "--json", program, "--strength", versions, *extra],
                             status))
    record = next(r for r in items if r["record"] == "strength")
    assert record["set"] == versions and record["version"] == 2, items
    return record


def strength_checks(binary):
    versions = f"{STRENGTH}/versions"
    record = strength_record(binary, f"{STRENGTH}/main.mo", versions)
    assert {v["name"]: v["outcome"] for v in record["versions"]} == STRENGTH_OUTCOMES, record
    assert (record["killed"], record["killed_any"], record["scored"], record["rate"],
            record["partial"], record["counted_file"], record["original"]) == (
                3, 3, 4, 0.75, False, False, "passed")
    assert [v.get("reason") for v in record["versions"] if v["outcome"] == "invalid"] == [
        "E0078: a function's result doesn't match its signature",
        "a declaration it replaces is missing, overloaded or replaced twice"], record
    assert strength_record(binary, f"{STRENGTH}/main.mo", versions) == record
    text = run(binary, ["test", f"{STRENGTH}/main.mo", "--strength", versions], 0).stdout
    assert (b"strength: 3 of 4 wrong versions killed (75.0%), 3 by any check; 2 invalid, "
            b"1 changes_intent, "
            b"1 defect, 0 unfinished, 0 not_counted\n") in text, text
    used = strength_record(binary, f"{STRENGTH}-json/main.mo", f"{STRENGTH}-json/versions")
    assert [(v["name"], v["outcome"]) for v in used["versions"]] == [
        ("a-wrong.mo", "killed_by_example")], used
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
    tests = strength_record(binary, f"{STRENGTH}-tests/checks.mo", f"{STRENGTH}-tests/versions")
    assert {v["name"]: v["outcome"] for v in tests["versions"]} == {
        "a-third.mo": "killed_by_test", "b-either.mo": "killed_by_promise",
        "c-floor.mo": "survived", "d-zero.mo": "killed_by_test", "e-stop.mo": "defect"}, tests
    assert (tests["killed"], tests["killed_any"], tests["scored"]) == (1, 3, 4), tests
    slow = strength_record(binary, f"{STRENGTH}-tests/checks.mo", f"{STRENGTH}-tests/slow", 1,
                           ["--seconds", "1"])
    assert ([v["outcome"] for v in slow["versions"]], slow["partial"], slow["killed_any"]) == (
        ["unfinished", "unfinished"], True, 0), slow
    service = strength_record(binary, f"{STRENGTH}-service/main.mo", f"{STRENGTH}-service/versions")
    assert ([v["outcome"] for v in service["versions"]], service["killed_any"],
            service["scored"]) == (["killed_by_test"], 1, 1), service
    for module in ("lib/list.mo", "lib/json.mo"):
        library = without_time(run(binary, ["test", "--list", "--json", module], 0))
        assert all("digest" not in r for r in library), library


def diff_command(binary):
    with tempfile.TemporaryDirectory(prefix="mo-diff-cli-") as directory:
        folder = pathlib.Path(directory)
        old_dir = folder / "old"
        new_dir = folder / "new"
        old_dir.mkdir()
        new_dir.mkdir()
        old = old_dir / "main.mo"
        new = new_dir / "main.mo"
        old.write_text(
            'module Main\n'
            'expose clamp\n'
            'limit = 10\n'
            'fn within?(n: Int) : Bool\n'
            '  n >= 0 and n <= limit\n'
            'end\n'
            'fn clamp(n: Int) : Int\n'
            '  promise\n'
            '    within?(result)\n'
            '  end\n'
            '  examples\n'
            '    clamp(3) == 3\n'
            '    clamp(20) == 10\n'
            '  end\n'
            '  if n < 0: 0 else if n > limit: limit else: n\n'
            'end\n'
            'fn main(net: Network, files: Files)\n'
            '  _n = net.allow_hosts(["api.github.com:443"])\n'
            '  _f = files.within("reports").read_only\n'
            'end\n'
        )
        # Refactoring that deletes private helper `within?` while preserving behavior and authority.
        new.write_text(
            'module Main\n'
            'expose clamp\n'
            'fn clamp(n: Int) : Int\n'
            '  return 0 if n < 0\n'
            '  return 10 if n > 10\n'
            '  n\n'
            'end\n'
            'fn main(net: Network, files: Files)\n'
            '  _n = net.allow_hosts(["api.github.com:443"])\n'
            '  _f = files.within("reports").read_only\n'
            'end\n'
        )
        ok_text = run(binary, ["diff", str(old), str(new)], 0).stdout.decode()
        assert "authority:\n  unchanged" in ok_text and "contracts: 3 passed, 0 failed" in ok_text, ok_text
        items = records(run(binary, ["diff", "--json", str(old), str(new)], 0), "diff")
        diff_rec = next(r for r in items if r["record"] == "diff")
        assert diff_rec["authority"]["capabilities"]["added"] == [], diff_rec
        assert diff_rec["authority"]["attenuations"]["added"] == [], diff_rec
        assert diff_rec["contracts"]["passed"] == 3 and diff_rec["contracts"]["failed"] == 0, diff_rec
        # Authority delta: widening Network.allow_hosts is reported in authority.attenuations.
        widened = folder / "widened"
        widened.mkdir()
        widened_main = widened / "main.mo"
        widened_main.write_text(new.read_text().replace(
            '["api.github.com:443"]', '["api.github.com:443", "evil.example:80"]'))
        bad_auth = records(run(binary, ["diff", "--json", str(old), str(widened_main)], 0), "diff")
        auth_rec = next(r for r in bad_auth if r["record"] == "diff")["authority"]
        assert any("evil.example:80" in a for a in auth_rec["attenuations"]["added"]), auth_rec
        # Behavioral regression: breaking old's promise exits 1.
        broken = folder / "broken"
        broken.mkdir()
        broken_main = broken / "main.mo"
        broken_main.write_text(new.read_text().replace("return 10 if n > 10", "return 99 if n > 10"))
        bad_beh = records(run(binary, ["diff", "--json", str(old), str(broken_main)], 1), "diff")
        contracts_rec = next(r for r in bad_beh if r["record"] == "diff")["contracts"]
        assert contracts_rec["failed"] >= 1, contracts_rec
        # Single-argument Git HEAD mode.
        repo = folder / "repo"
        repo.mkdir()
        import subprocess
        subprocess.run(["git", "init", "-q"], cwd=repo, check=True)
        subprocess.run(["git", "config", "user.email", "test@example.com"], cwd=repo, check=True)
        subprocess.run(["git", "config", "user.name", "Test"], cwd=repo, check=True)
        tracked = repo / "main.mo"
        tracked.write_text(old.read_text())
        subprocess.run(["git", "add", "main.mo"], cwd=repo, check=True)
        subprocess.run(["git", "commit", "-q", "-m", "initial"], cwd=repo, check=True)
        tracked.write_text(new.read_text())
        git_items = records(run(binary, ["diff", "--json", str(tracked)], 0), "diff")
        assert next(r for r in git_items if r["record"] == "diff")["contracts"]["passed"] == 3, git_items


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
