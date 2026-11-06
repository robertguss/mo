#!/usr/bin/env python3
"""mo history, mo replay and mo query against data folders a real mo serve wrote."""
import datetime
import hashlib
import json
import os
import pathlib
import shutil
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile

from check_cli import Served, records
from check_operator import TOKEN, client

root = pathlib.Path(__file__).resolve().parent.parent

FAILING = '''module Main
struct Box
  label: Text
  count: Int
end
service
  state Box = Box(label: "seed", count: 0)
  tools broken, add
  token from env "OPERATOR_TOKEN"
end
fn broken(state: Box, _now: Time, n: Int) : Bool
  if n > state.count
    stop "operator failure"
  end
  true
end
fn add(state: Box, _now: Time) : (Box, Bool)
  (Box(label: state.label, count: state.count + 1), true)
end
'''
GONE = "messages before the snapshot are gone, so a message absent from the log tail may still have run"


def fnv(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


class Message:
    """A version-1 failure message, field by field, as docs/design/introspection.md lays it out."""
    NUMBERS = (("version", 1), ("id", 8), ("sequence", 8), ("arrival", 8), ("timer", 1),
               ("handler", 4))

    def __init__(self, payload):
        self.at, self.p = 0, payload
        f = self.fields = {}
        for name, width in self.NUMBERS:
            f[name] = self.u(width)
        f["name"] = self.text()
        f["read_only"] = self.u(1)
        f["arguments"] = [self.text() for _ in range(self.u(4))]
        f["signature_version"], f["class"] = self.u(1), self.u(1)
        f["reason"], f["limit"], f["limit_value"] = self.text(), self.text(), self.u(8)
        for name in ("function", "file", "start", "end"):
            f[name] = self.u(4)
        for name in ("work", "heap", "stack", "depth", "saved", "program"):
            f[name] = self.u(8)
        f["build"], f["label"], f["schema"] = self.take(32), self.text(), self.take(32)
        f["context_status"], f["context"] = self.u(1), self.text()
        assert self.at == len(payload)

    def take(self, n):
        value = self.p[self.at:self.at + n]
        self.at += n
        return value

    def u(self, n):
        return int.from_bytes(self.take(n), "little")

    def text(self):
        return self.take(self.u(4))

    def bytes(self):
        f, out = self.fields, bytearray()

        def u(value, n):
            out.extend(value.to_bytes(n, "little"))

        def text(value):
            u(len(value), 4)
            out.extend(value)
        for name, width in self.NUMBERS:
            u(f[name], width)
        text(f["name"])
        u(f["read_only"], 1)
        u(len(f["arguments"]), 4)
        for argument in f["arguments"]:
            text(argument)
        u(f["signature_version"], 1)
        u(f["class"], 1)
        text(f["reason"])
        text(f["limit"])
        u(f["limit_value"], 8)
        for name in ("function", "file", "start", "end"):
            u(f[name], 4)
        for name in ("work", "heap", "stack", "depth", "saved", "program"):
            u(f[name], 8)
        out.extend(f["build"])
        text(f["label"])
        out.extend(f["schema"])
        u(f["context_status"], 1)
        text(f["context"])
        return bytes(out)


def edit_record(source, folder, name, failure, change):
    """A copy of the data folder with one field of one failure message changed, its frame's
    length and checksum made right again, so only the check that reads that field fails."""
    copy = folder / name
    shutil.copytree(source, copy)
    db = sqlite3.connect(copy / "service.sqlite")
    (frame,) = db.execute("SELECT message FROM failure WHERE id=?", (failure,)).fetchone()
    assert frame[:5] == b"MOST\1" and frame[5] == 4
    message = Message(frame[26:-8])
    change(message.fields)
    payload = message.bytes()
    header = frame[:22] + len(payload).to_bytes(4, "little")
    rebuilt = header + payload
    rebuilt += fnv(rebuilt).to_bytes(8, "little")
    db.execute("UPDATE failure SET message=? WHERE id=?", (rebuilt, failure))
    db.commit()
    db.close()
    return copy


def mo(binary, args, status):
    result = subprocess.run([str(binary), *args], cwd=root, capture_output=True, timeout=120)
    assert result.returncode == status, (args, result.returncode, result.stdout[-2000:],
                                         result.stderr[-2000:])
    return result


def serve(binary, folder, source, calls, fetches=()):
    """Run FILE as a service, make each call, fetch each (kind, id, path) export, and stop it
    with SIGTERM; return the data folder."""
    (folder / "main.mo").write_text(source)
    data = folder / "data"
    data.mkdir()
    env = {**os.environ, "OPERATOR_TOKEN": TOKEN}
    served = Served(binary, folder / "main.mo", data, env, folder)
    try:
        rpc, _ = client(served)
        for name, arguments in calls:
            status, _ = rpc("tools/call", {"name": name, "arguments": arguments})
            assert status == 200, (name, arguments)
        for kind, failure, path in fetches:
            selector = ["state"] if kind == "state" else ["failure", str(failure)]
            result = subprocess.run(
                [str(binary), "fetch", f"http://127.0.0.1:{served.port}/mcp", "--export",
                 *selector, "--out", str(path), "--token-env", "OPERATOR_TOKEN"],
                env=env, capture_output=True, timeout=300)
            assert result.returncode == 0, result.stderr
        served.finish(0, signal.SIGTERM)
    finally:
        served.close()
    return data


def history(binary, folder):
    data = serve(binary, folder, FAILING, [("broken", {"n": "5"}), ("add", {}), ("add", {}),
                                            ("broken", {"n": "9"})])
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    text = mo(binary, ["history", str(data)], 0)
    assert not text.stderr, text.stderr
    lines = text.stdout.decode().splitlines()
    assert lines[0] == f"history of {data}", lines
    assert lines[1] == ("snapshot: as of sequence 0, 2 later messages not applied; "
                        "producer build not recorded"), lines
    assert lines[2].startswith("state (") and '"label":"seed","count":"0"' in lines[2], lines
    assert lines[3] == "log tail: 2 messages after the snapshot", lines
    for line, sequence in zip(lines[4:6], (1, 2)):
        assert line.startswith(f"  {sequence}: tool 1 (name not recorded in the log), arrival "), line
        assert line.endswith(" us, 0 arguments"), line
    assert lines[6] == "note: " + GONE, lines
    assert lines[7] == "failures: 2 retained", lines
    for i, number in ((8, 1), (10, 2)):
        assert lines[i].startswith(f"  {number}: broken (tool), defect \"stop\" in function "), lines
        assert lines[i + 1].endswith(f"(sha256 {digest}); replayable here"), lines
    assert len(lines) == 12, lines

    items = records(mo(binary, ["history", "--json", str(data)], 0), "history")
    assert len(items) == 3, items
    h = items[1]
    assert h["record"] == "history" and h["folder"] == "ok" and h["data"] == str(data), h
    assert not h["recovered"] and h["head"] == "2" and h["log_entries"] == 2, h
    snapshot = h["snapshot"]
    assert snapshot["sequence"] == "0" and snapshot["later_messages"] == 2, snapshot
    assert snapshot["producer_build"] is None and snapshot["state"]["complete"], snapshot
    assert snapshot["state"]["value"]["fields"] == {"label": "seed", "count": "0"}, snapshot
    for entry, sequence in zip(h["log"], ("1", "2")):
        assert entry["handler"] is None and entry["handler_index"] == 1, entry
        assert entry["sequence"] == sequence and entry["timer"] is False, entry
        assert entry["argument_bytes"] == "0" and entry["argument_sizes"] == [], entry
        assert int(entry["arrival_us"]) > 0, entry
    assert h["log_note"] == GONE and h["failure_capture"] and h["failure_note"] is None, h
    assert h["failure_records"] == 2 and [f["id"] for f in h["failures"]] == ["1", "2"], h
    for f in h["failures"]:
        assert f["handler"] == "broken" and f["kind"].startswith("broken:0:stop:"), f
        assert f["signature"]["class"] == "defect" and f["signature"]["reason"] == "stop", f
        assert f["build"] == digest and f["replayable_here"] and f["not_replayable"] is None, f
        assert f["build_label"].startswith("mo 0.0.0; "), f
    assert h["left_bytes"] == "0" and int(h["retained_bytes"]) > 0, h
    return data


def owned(binary, folder, data):
    """A running service holds its store, so history refuses at once without waiting."""
    served = Served(binary, folder / "main.mo", data, {**os.environ, "OPERATOR_TOKEN": TOKEN},
                    folder)
    try:
        result = mo(binary, ["history", str(data)], 1)
        assert not result.stdout, result.stdout
        assert result.stderr == (f"mo history: a running service owns {data}; nothing was read, "
                                 "and nothing waited for it\n").encode(), result.stderr
        items = records(mo(binary, ["history", "--json", str(data)], 1), "history")
        assert items[1] == {"record": "history", "data": str(data), "folder": "owned",
                            "reason": f"a running service owns {data}; nothing was read, and "
                                      "nothing waited for it"}, items
        served.finish(0, signal.SIGTERM)
    finally:
        served.close()


HOT = '''
import os, sqlite3, sys
db = sqlite3.connect(sys.argv[1], isolation_level=None)
db.execute("PRAGMA cache_size=1")
db.execute("BEGIN")
db.execute("CREATE TABLE spill(x BLOB)")
db.execute("INSERT INTO spill VALUES (zeroblob(4000000))")
os._exit(0)
'''


def hot_journal(binary, folder, data):
    """A write killed after it reached the database leaves a journal that needs a rollback."""
    copy = folder / "hot"
    shutil.copytree(data, copy)
    path = copy / "service.sqlite"
    before = path.read_bytes()
    subprocess.run([sys.executable, "-I", "-c", HOT, str(path)], check=True, timeout=60)
    assert (copy / "service.sqlite-journal").stat().st_size > 0
    assert path.read_bytes() != before
    result = mo(binary, ["history", str(copy)], 1)
    assert result.stderr.startswith(f"mo history: recovery required in {copy}: ".encode()), result
    assert b"--recover" in result.stderr and not result.stdout, result
    items = records(mo(binary, ["history", "--json", str(copy)], 1), "history")
    assert items[1]["folder"] == "recovery_required", items
    recovered = records(mo(binary, ["history", "--json", "--recover", str(copy)], 0), "history")
    assert recovered[1]["recovered"] and recovered[1]["failure_records"] == 2, recovered
    text = mo(binary, ["history", str(copy)], 0).stdout.decode()
    assert text.startswith(f"history of {copy}\n"), text


def pre_capture(binary, folder, data):
    """A store from before failure capture has no failure tables; history creates none."""
    copy = folder / "old"
    shutil.copytree(data, copy)
    db = sqlite3.connect(copy / "service.sqlite")
    db.executescript("DROP TABLE failure; DROP TABLE failure_state")
    db.close()
    before = (copy / "service.sqlite").read_bytes()
    text = mo(binary, ["history", str(copy)], 0).stdout.decode()
    assert ("failures: none; this store predates failure capture, so it holds no failure "
            "records\n") in text, text
    h = records(mo(binary, ["history", "--json", str(copy)], 0), "history")[1]
    assert not h["failure_capture"] and h["failures"] == [] and h["failure_records"] == 0, h
    assert h["failure_note"] == "this store predates failure capture, so it holds no failure records"
    assert h["log_entries"] == 2, h
    assert (copy / "service.sqlite").read_bytes() == before


def missing(binary, folder):
    empty = folder / "empty"
    empty.mkdir()
    result = mo(binary, ["history", str(empty)], 1)
    assert result.stderr == f"mo history: no saved data in {empty}\n".encode(), result.stderr
    assert not (empty / "service.sqlite").exists()


def program(folder, name, source):
    path = folder / name / "main.mo"
    path.parent.mkdir()
    path.write_text(source)
    return str(path)


def replay_args(file, data, failure="1", *extra):
    return ["replay", str(file), "--failure", failure, "--data", str(data), *extra]


def refused(binary, args, check, *pieces):
    """A refusal: exit 1, nothing on stdout, one stderr line naming the check, and, as JSON, the
    outcome record alone."""
    command = args[0]
    result = mo(binary, args, 1)
    assert not result.stdout, result.stdout
    line = result.stderr.decode()
    assert line.startswith(f"mo {command}: refused: {check}: ") and line.count("\n") == 1, line
    for piece in pieces:
        assert piece in line, (piece, line)
    items = records(mo(binary, [command, "--json", *args[1:]], 1), command)
    record = items[1]
    assert record["outcome" if "outcome" in record else "verdict"] == "refused", items
    assert record["refusal"]["check"] == check and "run" not in record, record
    return record["refusal"]


def replays(binary, folder, data, artifacts):
    file = folder / "main.mo"
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    # A captured stop, replayed from the stopped folder and from its fetched artifact.
    text = mo(binary, replay_args(file, data), 0)
    assert not text.stderr, text.stderr
    lines = text.stdout.decode().splitlines()
    assert lines[0].startswith("replay of failure 1: broken (read-only tool 0, 1 argument), "
                               "sequence 0, arrival "), lines
    assert lines[1] == 'state: Box(label: "seed", count: 0)' and lines[2] == "argument 1: 5", lines
    assert lines[3].startswith('recorded: defect "stop" in function '), lines
    offline = next(i for i, line in enumerate(lines) if line.startswith("offline: "))
    assert lines[offline] == "offline: " + lines[3][len("recorded: "):], lines
    assert lines[offline + 1] == "stack (complete):" and "variable n available: 5" in text.stdout.decode()
    assert lines[-1] == "outcome: reproduced", lines
    both = []
    for source in (["--data", str(data)], ["--export", str(artifacts["failure 1"])]):
        items = records(mo(binary, ["replay", "--json", str(file), "--failure", "1", *source], 0),
                        "replay")
        assert [item["record"] for item in items] == ["version", "original", "totals"], items
        original = items[1]
        assert original["outcome"] == "reproduced" and original["handler"] == "broken", original
        assert original["state"] == {"value": {"type": "Main.Box",
                                               "fields": {"label": "seed", "count": "0"}},
                                     "complete": True}, original
        assert original["arguments"] == [{"value": "5", "complete": True}], original
        assert original["run"]["status"] == "defect" and original["note"] is None, original
        assert original["run"]["signature"] == original["recorded"]["signature"], original
        assert original["run"]["context_status"] == "complete", original
        both.append(original["run"]["signature"])
    assert both[0] == both[1] and both[0]["reason"] == "stop", both

    # A record whose span was changed stops elsewhere offline: a different failure.
    moved = edit_record(data, folder, "moved", 1, lambda f: f.update(start=f["start"] + 1))
    result = mo(binary, replay_args(file, moved), 1)
    assert result.stdout.decode().endswith("outcome: different\n"), result.stdout

    # Each check refused by a fixture that breaks only that check.
    kind = refused(binary, ["replay", str(file), "--failure", "1", "--export",
                            str(artifacts["state"])], "kind", "holds an exported state")
    assert kind["recorded"] == "state" and kind["found"] == "failure", kind
    other = refused(binary, ["replay", str(file), "--failure", "2", "--export",
                             str(artifacts["failure 1"])], "failure")
    assert other["recorded"] == "1" and other["found"] == "2", other
    broken = folder / "broken.moex"
    broken.write_bytes(artifacts["failure 1"].read_bytes()[:-1])
    refused(binary, ["replay", str(file), "--failure", "1", "--export", str(broken)], "format")
    elsewhere = edit_record(data, folder, "elsewhere", 1,
                            lambda f: f.update(build=bytes(31) + b"\1"))
    build = refused(binary, replay_args(file, elsewhere), "build", "recorded by another build",
                    "sha256sum")
    assert build["recorded"].endswith("(sha256 " + "00" * 31 + "01)"), build
    assert build["found"].endswith(f"(sha256 {digest})"), build
    unnamed = edit_record(data, folder, "unnamed", 1, lambda f: f.update(build=bytes(32)))
    refused(binary, replay_args(file, unnamed), "build",
            "the record does not identify the build that recorded it")
    listed = records(mo(binary, ["history", "--json", str(unnamed)], 0), "history")[1]
    assert [(f["id"], f["replayable_here"], f["not_replayable"]) for f in listed["failures"]] == [
        ("1", False, "the record does not identify the build that recorded it"),
        ("2", True, None)], listed["failures"]
    listed = records(mo(binary, ["history", "--json", str(elsewhere)], 0), "history")[1]
    assert listed["failures"][0]["not_replayable"] == "recorded by another build", listed
    edited = program(folder, "edited", "# one comment\n" + FAILING)
    refused(binary, replay_args(edited, data), "program", "must be the untouched source")
    renamed = edit_record(data, folder, "renamed", 1, lambda f: f.update(name=b"broke"))
    handler = refused(binary, replay_args(file, renamed), "handler")
    assert handler["recorded"] == "broke (read-only tool 0, 1 argument)", handler
    assert handler["found"] == "broken (read-only tool 0, 1 argument)", handler
    schema = edit_record(data, folder, "schema", 1, lambda f: f.update(schema=bytes(32)))
    state = refused(binary, replay_args(file, schema), "state_schema")
    assert state["recorded"] == "00" * 32 and len(state["found"]) == 64, state
    text_argument = b"MOSV\1" + struct.pack("<I", 13) + struct.pack("<BIII", 10, 0, 1, 0) + \
        struct.pack("<I", 1) + b"5"
    retyped = edit_record(data, folder, "retyped", 1,
                          lambda f: f.update(arguments=[text_argument]))
    argument = refused(binary, replay_args(file, retyped), "argument_schema", "argument 1")
    assert argument["recorded"] == hashlib.sha256(struct.pack("<BIII", 10, 0, 1, 0)).hexdigest()
    budgets = edit_record(data, folder, "budgets", 1, lambda f: f.update(work=f["work"] + 1))
    budget = refused(binary, replay_args(file, budgets), "budgets", "damaged or crafted")
    assert budget["recorded"] == "work = 1048577" and budget["found"] == "work = 1048576", budget
    evicted = refused(binary, replay_args(file, data, "3"), "failure", "replaced or evicted")
    assert evicted["found"] == "1, 2", evicted

    # The fix check: a fixed handler is verified, the unchanged one is not_fixed, and a fix of
    # an original that did not reproduce is unverified.
    fixed = program(folder, "fixed", FAILING.replace('    stop "operator failure"\n',
                                                     "    return false\n"))
    same = program(folder, "same", FAILING)
    text = mo(binary, replay_args(file, data, "1", "--fix", fixed), 0).stdout.decode()
    assert text.endswith("outcome: reproduced\nrevised: succeeded, reply ok, state unchanged\n"
                         "verdict: verified\n"), text
    items = records(mo(binary, ["replay", "--json", *replay_args(file, data, "1", "--fix",
                                                                  fixed)[1:]], 0), "replay")
    assert [item["record"] for item in items] == ["version", "original", "revised", "totals"]
    assert items[2]["verdict"] == "verified" and items[2]["run"]["reply"] == "ok", items
    assert items[2]["run"]["state_changed"] is False and items[2]["handler"] == "broken", items
    text = mo(binary, replay_args(file, data, "1", "--fix", same), 1).stdout.decode()
    assert text.endswith("verdict: not_fixed\n"), text
    text = mo(binary, replay_args(file, moved, "1", "--fix", fixed), 1).stdout.decode()
    assert "outcome: different\n" in text and text.endswith("verdict: unverified\n"), text
    # Both programs are checked before either runs: a revision that changes the state type is
    # refused and nothing replays.
    grown = program(folder, "grown", FAILING.replace("  count: Int\n", "  count: Int\n  extra: Int\n")
                    .replace('Box(label: "seed", count: 0)', 'Box(label: "seed", count: 0, extra: 0)')
                    .replace("count: state.count + 1)", "count: state.count + 1, extra: 0)"))
    refused(binary, replay_args(file, data, "1", "--fix", grown), "revised_state_schema",
            "the fix changes the state type")
    updating = program(folder, "updating", FAILING.replace(
        "fn broken(state: Box, _now: Time, n: Int) : Bool", "fn broken(state: Box, _now: Time, n: Int) : (Box, Bool)")
        .replace("    return false\n", "").replace('    stop "operator failure"\n  end\n  true',
                                                     '    stop "operator failure"\n  end\n  (state, true)'))
    shape = refused(binary, replay_args(file, data, "1", "--fix", updating), "revised_handler",
                    "shape differs")
    assert shape["recorded"] == "broken (read-only tool 0, 1 argument)", shape
    assert shape["found"] == "broken (updating tool 0, 1 argument)", shape


SPIN = '''module Main
service
  state Int = 0
  tools spin
  token from env "OPERATOR_TOKEN"
end
fn spin(state: Int, _now: Time, n: Int) : Bool
  var total = state
  for i in 0..n
    total += i
  end
  total > 0
end
'''


def work_limit(binary, folder):
    """A handler cancelled by its work budget replays to the same work signature; a revision
    that still runs past the recorded budget is not fixed."""
    data = serve(binary, folder, SPIN, [("spin", {"n": "3000000"})])
    file = folder / "main.mo"
    items = records(mo(binary, ["replay", "--json", *replay_args(file, data)[1:]], 0), "replay")
    run = items[1]["run"]
    assert items[1]["outcome"] == "reproduced" and run["status"] == "work", items
    assert run["signature"]["class"] == "work" and run["signature"]["value"] == "1048576", run
    slower = program(folder, "slower", SPIN.replace("for i in 0..n", "for i in 0..(n / 2)"))
    text = mo(binary, replay_args(file, data, "1", "--fix", slower), 1).stdout.decode()
    assert 'revised: work "work limit" at MO_LIMIT_SERVICE_WORK = 1048576\n' in text, text
    assert text.endswith("verdict: not_fixed\n"), text
    quick = program(folder, "quick", SPIN.replace("for i in 0..n", "for i in 0..(n / 10000)"))
    text = mo(binary, replay_args(file, data, "1", "--fix", quick), 0).stdout.decode()
    assert text.endswith("revised: succeeded, reply ok, state unchanged\nverdict: verified\n"), text


QUERIES = '''
fn label(state: Box) : Text
  state.label
end
fn inputs(state: Box, now: Time, n: Int) : Text
  "#{state.count} #{n} #{now}"
end
fn swapped(_state: Box, n: Int, now: Time) : Text
  "#{n} #{now}"
end
fn spin(_state: Box, _now: Time, n: Int) : Int
  var total = 0
  for i in 0..n
    total += i
  end
  total
end
fn grow(_state: Box) : Int
  var items = []
  for i in 0..1000000
    items = items.append("#{i}")
  end
  items.size
end
fn deep(state: Box, n: Int) : Int
  if n == 0
    return 0
  end
  1 + deep(state, n - 1)
end
fn depth(state: Box) : Int
  deep(state, 100000)
end
'''


def queries(binary, folder, data, artifacts):
    file = program(folder, "query", FAILING + QUERIES)
    state = ["--export", str(artifacts["state"])]
    # A program that only adds functions reads the exported state.
    result = mo(binary, ["query", file, "label", *state], 0)
    assert result.stdout == b"seed\n" and not result.stderr, result
    items = records(mo(binary, ["query", "--json", file, "label", *state], 0), "query")
    assert items[1] == {"record": "query", "function": "label", "outcome": "returned",
                        "result": {"value": "seed", "complete": True}}, items
    renamed = program(folder, "renamed-field", (FAILING + QUERIES).replace("label: Text", "name: Text")
                      .replace('label: "seed"', 'name: "seed"').replace("label: state.label", "name: state.name")
                      .replace("  state.label\n", "  state.name\n"))
    schema = refused(binary, ["query", renamed, "label", *state], "state_schema")
    assert len(schema["recorded"]) == len(schema["found"]) == 64, schema
    assert schema["recorded"] != schema["found"], schema
    # Over a failure, the function gets the record's state, arrival and arguments in order.
    listed = records(mo(binary, ["history", "--json", str(data)], 0), "history")[1]
    arrival = int(listed["failures"][1]["arrival"])
    when = datetime.datetime.fromtimestamp(arrival / 1e6, datetime.timezone.utc)
    fraction = when.strftime("%f").rstrip("0")  # reference 14: as short as it can be
    stamp = when.strftime("%Y-%m-%dT%H:%M:%S") + ("." + fraction if fraction else "") + "Z"
    result = mo(binary, ["query", file, "inputs", "--data", str(data), "--failure", "2"], 0)
    assert result.stdout.decode() == f"2 9 {stamp}\n", (result.stdout, stamp)
    result = mo(binary, ["query", file, "inputs", "--export", str(artifacts["failure 1"]),
                         "--failure", "1"], 0)
    assert result.stdout.decode().startswith("0 5 "), result.stdout
    # A function whose parameters reorder the handler's inputs is refused before anything runs.
    result = mo(binary, ["query", file, "swapped", "--data", str(data), "--failure", "2"], 1)
    assert b"E1413" in result.stderr and not result.stdout, result
    refused(binary, ["query", file, "label", "--export", str(artifacts["failure 1"])], "kind",
            "holds a failure")
    result = mo(binary, ["query", file, "nothing", *state], 2)
    assert result.stderr == b"mo query: no function of that name in that module\n", result
    assert not result.stdout and not mo(binary, ["query", "--json", file, "nothing", *state],
                                        2).stdout
    # A query that runs out of a limit stops with mo run's report naming it, and exits 70.
    for function, extra, limit in (("spin", ["--max-work", "10"], "--max-work"),
                                   ("grow", ["--max-heap", "1MiB"], "--max-heap"),
                                   ("depth", ["--max-depth", "50"], "--max-depth"),
                                   ("depth", ["--max-stack", "64KiB"], "--max-stack")):
        args = [file, function, *extra]
        args += ["--data", str(data), "--failure", "2"] if function == "spin" else state
        result = mo(binary, ["query", *args], 70)
        assert not result.stdout and result.stderr.startswith(b"bug: "), result
        assert limit.encode() in result.stderr, (limit, result.stderr)
        result = mo(binary, ["query", "--json", *args], 70)
        lines = [json.loads(line) for line in result.stdout.decode().splitlines()]
        assert lines[1]["outcome"] == "stopped" and lines[1]["limit"] == limit, lines
        assert lines[-1] == {"record": "totals", "errors": 0, "dropped": 0, "status": 70}, lines
    result = mo(binary, ["query", file, "spin", "--data", str(data), "--failure", "2",
                         "--max-work", "100000"], 0)
    assert result.stdout == b"36\n", result.stdout


# The agent guide's claims about these commands, each exercised above.
GUIDE = [
    "shows the last snapshot \"as of sequence S, K later messages not applied\", the log tail's "
    "sequence, handler index, arrival and argument sizes, and each retained failure with its build "
    "and whether this build can replay it.",  # history
    "says \"name not recorded in the log\"",  # history
    "A store from before failure capture says \"this store predates failure capture\".",  # pre_capture
    "refuses at once, with its own message, a folder a running service owns",  # owned, missing
    "run it again as the folder's owner with `--recover`, which writes database pages but changes "
    "no record.",  # hot_journal
    "It prints the original message, the pre-call state, the recorded and offline signatures and "
    "the regenerated stack with the top frame's variables.",  # replays
    "any byte change, a comment or `mo format` included, changes the program identity and is "
    "refused.",  # replays: edited
    "The outcome is `reproduced` (exit 0), or `different`, `not_reproduced` or `refused` (exit 1).",
    "a fix that changes the state type, or the handler's name, kind, arguments or result shape, is "
    "refused.",  # replays: grown, updating
    "The verdict is `verified` (exit 0) only when the original reproduced and the revision "
    "succeeded within the recorded budgets; otherwise it is `not_fixed`, `unverified` or `refused` "
    "(exit 1).",  # replays, work_limit
    "`FILE` may add functions or modules to the deployed program, but the state type must not "
    "change",  # queries
    "a query that stops on one exits 70 with a report that names the limit.",  # queries
    "A refusal prints one `mo replay: refused: CHECK: ...` line, or `mo query: refused:`, naming "
    "what differs with its recorded and found values, and exits 1.",  # refused
    "`--json` writes a version record, one record per outcome (`original`, `revised`, `query` or "
    "`history`), and a totals record whose `status` is the exit status.",
]


def guide(binary):
    text = " ".join(mo(binary, ["guide"], 0).stdout.decode().split())
    for sentence in GUIDE:
        assert sentence in text, f"docs/agent-guide.md no longer says: {sentence}"


def check(binary):
    guide(binary)
    with tempfile.TemporaryDirectory(prefix="mo-offline-") as directory:
        folder = pathlib.Path(directory)
        data = history(binary, folder)
        owned(binary, folder, data)
        artifact_folder = folder / "fetched"
        artifact_folder.mkdir()
        artifacts = {"failure 1": artifact_folder / "failure-1.moex",
                     "state": artifact_folder / "state.moex"}
        fetched = folder / "fetching"
        fetched.mkdir()
        fetched_data = serve(binary, fetched, FAILING, [("broken", {"n": "5"})],
                             [("failure", 1, artifacts["failure 1"]),
                              ("state", None, artifacts["state"])])
        replays(binary, folder, data, artifacts)
        queries(binary, folder, data, artifacts)
        spinning = folder / "spinning"
        spinning.mkdir()
        work_limit(binary, spinning)
        hot_journal(binary, folder, data)
        pre_capture(binary, folder, data)
        missing(binary, folder)
    print(f"{binary}: offline history, replay and query checks passed")


if __name__ == "__main__":
    for executable in sys.argv[1:]:
        check(pathlib.Path(executable).resolve())
