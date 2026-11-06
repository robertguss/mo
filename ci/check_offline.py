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
import threading
import time

from check_cli import Served, records
from check_operator import TOKEN, client

root = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root / "learnability" / "phase4"))
import fixture  # noqa: E402  the independent reader of a stopped store's log

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
# A saved Text value "5", and its schema: a value no Box or Int decodes from.
TEXT_SCHEMA = struct.pack("<BIII", 10, 0, 1, 0)
TEXT_SAVED = b"MOSV\1" + struct.pack("<I", 13) + TEXT_SCHEMA + struct.pack("<I", 1) + b"5"
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


def edit_state(source, folder, name, failure, payload):
    """A copy of the data folder whose failure's pre-call state holds another payload, framed
    again with the message's sequence and image and a right checksum, so only the binding of the
    state to its record can refuse it."""
    copy = folder / name
    shutil.copytree(source, copy)
    db = sqlite3.connect(copy / "service.sqlite")
    (frame,) = db.execute("SELECT record FROM failure_state WHERE id=?", (failure,)).fetchone()
    assert frame[:5] == b"MOST\1" and frame[5] == 3
    rebuilt = frame[:22] + len(payload).to_bytes(4, "little") + payload
    rebuilt += fnv(rebuilt).to_bytes(8, "little")
    db.execute("UPDATE failure_state SET record=? WHERE id=?", (rebuilt, failure))
    db.commit()
    db.close()
    return copy


def mo(binary, args, status, timeout=120):
    result = subprocess.run([str(binary), *args], cwd=root, capture_output=True, timeout=timeout)
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


CONTROLS = r'''module Main
service
  state Text = "a\u{9B}b\u{202E}c\u{7F}"
  tools get
  token from env "OPERATOR_TOKEN"
end
fn get(state: Text, _now: Time) : Int
  state.char_count
end
'''


def history_controls(binary, folder):
    """The text form escapes the snapshot's C1, bidi and DEL controls as every other value it
    prints is escaped; --json keeps them as JSON strings."""
    folder.mkdir()
    data = serve(binary, folder, CONTROLS, [])
    text = mo(binary, ["history", str(data)], 0).stdout
    for raw in ("\u009b", "\u202e", "\u007f"):
        assert raw.encode() not in text, text
    state = next(line for line in text.decode().splitlines() if line.startswith("state ("))
    assert '"a\\u{09b}b\\u{202e}c\\x7f"' in state, state
    h = records(mo(binary, ["history", "--json", str(data)], 0), "history")[1]
    assert h["snapshot"]["state"] == {"value": "a\u009bb\u202ec\u007f", "complete": True}, h


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


def hot_copy(folder, data, name):
    copy = folder / name
    shutil.copytree(data, copy)
    path = copy / "service.sqlite"
    before = path.read_bytes()
    subprocess.run([sys.executable, "-I", "-c", HOT, str(path)], check=True, timeout=60)
    assert (copy / "service.sqlite-journal").stat().st_size > 0
    assert path.read_bytes() != before
    return copy


def hot_journal(binary, folder, data):
    """A write killed after it reached the database leaves a journal that needs a rollback;
    replay and query take --recover as history does."""
    for command in ("replay", "query"):
        held = hot_copy(folder, data, f"hot-{command}")
        args = (replay_args(folder / "main.mo", held) if command == "replay" else
                ["query", str(folder / "query" / "main.mo"), "inputs", "--data", str(held),
                 "--failure", "1"])
        refused(binary, args, "folder", f"recovery required in {held}", "--recover")
        assert mo(binary, [*args, "--recover"], 0).stdout, command
    copy = hot_copy(folder, data, "hot")
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


def refused(binary, args, check, *pieces, timeout=120):
    """A refusal: exit 1, nothing on stdout, one stderr line naming the check, and, as JSON, the
    outcome record alone."""
    command = args[0]
    result = mo(binary, args, 1, timeout)
    assert not result.stdout, result.stdout
    line = result.stderr.decode()
    assert line.startswith(f"mo {command}: refused: {check}: ") and line.count("\n") == 1, line
    for piece in pieces:
        assert piece in line, (piece, line)
    items = records(mo(binary, [command, "--json", *args[1:]], 1, timeout), command)
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
                             str(artifacts["failure 1"])], "failure", "; recorded 1; asked for 2\n")
    assert other["recorded"] == "1" and other["found"] == "2", other
    # An artifact path that is not a regular file is refused at once with that reason: a FIFO
    # is never opened for a blocking read.
    fifo = folder / "artifact.fifo"
    os.mkfifo(fifo)
    for path in (folder, fifo):
        unread = refused(binary, ["replay", str(file), "--failure", "1", "--export", str(path)],
                         "format", "the artifact cannot be read; found not a regular file",
                         timeout=10)
        assert unread["found"] == "not a regular file", unread
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
    # A record whose schema is not its state's is damaged, as an artifact's would be; a state
    # and record that agree on another type reach the state type check.
    schema = edit_record(data, folder, "schema", 1, lambda f: f.update(schema=bytes(32)))
    refused(binary, replay_args(file, schema), "format", "does not belong to its message")
    restated = edit_record(edit_state(data, folder, "restated-state", 1, TEXT_SAVED), folder,
                           "restated", 1,
                           lambda f: f.update(schema=hashlib.sha256(TEXT_SCHEMA).digest()))
    state = refused(binary, replay_args(file, restated), "state_schema")
    assert state["recorded"] == hashlib.sha256(TEXT_SCHEMA).hexdigest(), state
    assert len(state["found"]) == 64 and state["found"] != state["recorded"], state
    retyped = edit_record(data, folder, "retyped", 1, lambda f: f.update(arguments=[TEXT_SAVED]))
    argument = refused(binary, replay_args(file, retyped), "argument_schema", "argument 1")
    assert argument["recorded"] == hashlib.sha256(TEXT_SCHEMA).hexdigest()
    budgets = edit_record(data, folder, "budgets", 1, lambda f: f.update(work=f["work"] + 1))
    budget = refused(binary, replay_args(file, budgets), "budgets", "damaged or crafted")
    assert budget["recorded"] == "work = 1048577" and budget["found"] == "work = 1048576", budget
    evicted = refused(binary, replay_args(file, data, "3"), "failure", "replaced or evicted",
                      "; retained ids 1, 2\n")
    assert evicted["found"] == "1, 2", evicted
    # A pre-call state framed for this message but of another schema is refused as damaged
    # before anything decodes it.
    foreign = edit_state(data, folder, "foreign-state", 1, TEXT_SAVED)
    refused(binary, replay_args(file, foreign), "format", "does not belong to its message")

    # A succeeding handler does not reproduce; only a recorded memory failure gets the note
    # that live heap occupants are not reconstructed.
    for name, signature, note in (("memory", 2, True), ("defect", 0, False)):
        succeeds = edit_record(data, folder, f"succeeds-{name}", 1, lambda f, c=signature: f.update(
            {"handler": 1, "name": b"add", "read_only": 0, "arguments": [], "class": c}))
        text = mo(binary, replay_args(file, succeeds), 1).stdout.decode()
        assert ("offline: succeeded, reply ok, state changed\n"
                "outcome: not_reproduced\n") in text, text
        assert text.endswith("note: live heap occupants, such as arguments queued in the mailbox, "
                             "are not reconstructed offline, so a memory failure that depended on "
                             "them does not recur\n") == note, text
        original = records(mo(binary, ["replay", "--json", *replay_args(file, succeeds)[1:]], 1),
                           "replay")[1]
        assert original["outcome"] == "not_reproduced", original
        assert (original["note"] is not None) == note, original
        assert original["note"] is None or original["note"].startswith("live heap occupants")

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
    # A revision that does not compile is refused after its own diagnostics, before the original
    # runs.
    uncompiled = program(folder, "uncompiled",
                         FAILING.replace("state.count + 1", "state.count + true"))
    for json_form in (False, True):
        args = replay_args(file, data, "1", "--fix", uncompiled)
        result = mo(binary, [args[0], *(["--json"] if json_form else []), *args[1:]], 1)
        errors = result.stderr.decode()
        assert "error E0053: " in errors and errors.endswith(
            "" if json_form else "mo replay: refused: revised_program: REVISED does not compile; "
            "mo check REVISED shows why\n"), errors
        if json_form:
            items = [json.loads(line) for line in result.stdout.decode().splitlines()]
            assert [i["record"] for i in items] == ["version", "original", "revised",
                                                    "totals"], items
            assert items[1]["outcome"] == "refused" and "run" not in items[1], items
            assert items[1]["refusal"]["check"] == "revised_program", items
        else:
            assert not result.stdout, result.stdout
    serviceless = program(folder, "serviceless", FAILING.replace(
        'service\n  state Box = Box(label: "seed", count: 0)\n  tools broken, add\n'
        '  token from env "OPERATOR_TOKEN"\nend\n', ""))
    refused(binary, replay_args(file, data, "1", "--fix", serviceless), "revised_program",
            "REVISED's main module declares no service")
    unnamed_fix = program(folder, "unnamed-fix", FAILING.replace("tools broken, add",
                                                                 "tools broke, add")
                          .replace("fn broken(", "fn broke("))
    missing_handler = refused(binary, replay_args(file, data, "1", "--fix", unnamed_fix),
                              "revised_handler", "has no tool of the recorded handler's name")
    assert missing_handler["recorded"] == "broken (read-only tool 0, 1 argument)", missing_handler
    assert missing_handler["found"] == "none", missing_handler
    retyped_fix = program(folder, "retyped-fix", FAILING.replace(
        "n: Int) : Bool", "n: Text) : Bool").replace("if n > state.count",
                                                     "if n.char_count > state.count"))
    refused(binary, replay_args(file, data, "1", "--fix", retyped_fix), "revised_handler",
            "argument 1's type differs from the recorded one")


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
    fifo = folder / "query.fifo"
    os.mkfifo(fifo)
    refused(binary, ["query", file, "label", "--export", str(fifo)], "format",
            "not a regular file", timeout=10)
    # The build is checked right after the input is read, before FILE's handler is looked up.
    foreign = edit_record(data, folder, "foreign-build", 1,
                          lambda f: f.update(build=bytes(31) + b"\1"))
    lacking = program(folder, "lacking",
                      (FAILING + QUERIES).replace("tools broken, add", "tools add"))
    refused(binary, ["query", lacking, "inputs", "--data", str(foreign), "--failure", "1"], "build",
            "recorded by another build")
    # A failure whose state is not the record's is refused before any decode, never reported
    # as a --max-heap stop.
    foreign_state = edit_state(data, folder, "foreign-query-state", 1, TEXT_SAVED)
    refused(binary, ["query", file, "inputs", "--data", str(foreign_state), "--failure", "1"],
            "format", "does not belong to its message")
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


GAUGE = '''module Main
struct Gauge
  armed: Bool
  count: Int
end
service
  state Gauge = Gauge(armed: false, count: 0)
  tools peek, bump, arm, disarm
  every 1.second tick
  token from env "OPERATOR_TOKEN"
end
## Reads the count plus n, and fails past 3.
fn peek(state: Gauge, _now: Time, n: Int) : Int
  if n > 3
    stop "peek failure"
  end
  state.count + n
end
## Adds n to the count, and fails past 3.
fn bump(state: Gauge, _now: Time, n: Int) : (Gauge, Int)
  if n > 3
    stop "bump failure"
  end
  (Gauge(armed: state.armed, count: state.count + n), state.count + n)
end
## Makes the timer fail.
fn arm(state: Gauge, _now: Time) : (Gauge, Bool)
  (Gauge(armed: true, count: state.count), true)
end
## Stops the timer failing.
fn disarm(state: Gauge, _now: Time) : (Gauge, Bool)
  (Gauge(armed: false, count: state.count), true)
end
fn tick(state: Gauge, _now: Time) : Gauge
  if state.armed
    stop "tick failure"
  end
  state
end
'''
GAUGE_QUERIES = '''
fn count(state: Gauge) : Int
  state.count
end
fn inputs(state: Gauge, _now: Time, n: Int) : Text
  "#{state.count} #{n}"
end
'''
READER_HOLD = 3.0  # seconds a reader keeps the store while mo serve starts; the wait is 10


def hold_store(path, holding, seconds):
    """A reader from Python's sqlite3: a shared lock on the store for `seconds`."""
    db = sqlite3.connect(f"file:{path}?mode=ro", uri=True, isolation_level=None)
    try:
        db.execute("BEGIN")
        db.execute("SELECT count(*) FROM log").fetchone()
        holding.set()
        time.sleep(seconds)
        db.execute("COMMIT")
    finally:
        db.close()


def serve_gauge(binary, folder, artifacts):
    """Serve GAUGE, make its read-only tool, its updating tool and its timer each fail, fetch
    every failure and the state, and check that history refuses the live folder at once;
    returns the data folder and the records mo-failures listed."""
    data = folder / "data"
    data.mkdir()
    served = Served(binary, folder / "deployed" / "main.mo", data,
                    {**os.environ, "OPERATOR_TOKEN": TOKEN}, folder)
    try:
        rpc, call = client(served)
        for name, arguments, failed in (("bump", {"n": "2"}, False), ("peek", {"n": "5"}, True),
                                        ("bump", {"n": "7"}, True), ("arm", {}, False)):
            status, body = rpc("tools/call", {"name": name, "arguments": arguments})
            assert status == 200 and body["result"]["isError"] is failed, (name, body)
        # The timer ticks every second; disarm it once its first failure is retained, well
        # before five failures in a minute would stop the service.
        deadline = time.monotonic() + 20
        while not any(r["handler"] == "tick" for r in call("mo-failures")["records"]):
            assert time.monotonic() < deadline, "the armed timer never failed"
            time.sleep(0.1)
        call("disarm")
        live = call("mo-failures")
        assert live["next"] is None and not live["unavailable"], live
        ids = {}
        for record in live["records"]:
            ids.setdefault(record["handler"], record["id"])
        assert sorted(ids) == ["bump", "peek", "tick"], live
        env = {**os.environ, "OPERATOR_TOKEN": TOKEN}
        for selector, path in [(["failure", ids[h]], artifacts[h]) for h in ids] + \
                              [(["state"], artifacts["state"])]:
            result = subprocess.run(
                [str(binary), "fetch", f"http://127.0.0.1:{served.port}/mcp", "--export",
                 *selector, "--out", str(path), "--token-env", "OPERATOR_TOKEN"],
                env=env, capture_output=True, timeout=300)
            assert result.returncode == 0, result.stderr
        # A running service owns its folder: history refuses at once and waits for nothing.
        began = time.monotonic()
        result = mo(binary, ["history", str(data)], 1)
        assert time.monotonic() - began < 3, time.monotonic() - began
        assert result.stderr == (f"mo history: a running service owns {data}; nothing was read, "
                                 "and nothing waited for it\n").encode(), result.stderr
        served.finish(0, signal.SIGTERM)
    finally:
        served.close()
    return data, ids, live["records"]


def serve_under_reader(binary, folder, data):
    """A reader holding the store across a mo serve start delays it; the service starts once
    the reader closes, inside the store wait, and recovers the same state."""
    copy = folder / "held"
    shutil.copytree(data, copy)
    holding = threading.Event()
    reader = threading.Thread(target=hold_store,
                              args=(copy / "service.sqlite", holding, READER_HOLD))
    reader.start()
    assert holding.wait(timeout=30)
    began = time.monotonic()
    served = Served(binary, folder / "deployed" / "main.mo", copy,
                    {**os.environ, "OPERATOR_TOKEN": TOKEN}, folder)
    try:
        waited = time.monotonic() - began
        reader.join(timeout=30)
        assert served.port, served.errors()
        assert READER_HOLD - 0.5 < waited < 9, waited
        rpc, _ = client(served)
        _, body = rpc("tools/call", {"name": "peek", "arguments": {"n": "0"}})
        assert body["result"]["structuredContent"] == {"value": "2"}, body
        served.finish(0, signal.SIGTERM)
    finally:
        served.close()


def gauge_history(binary, copy, live):
    """History of the stopped folder, checked against fixture.py's independent reader of the
    store and against what mo-failures listed while the service ran."""
    snapshot, rows = fixture.log_records(copy)
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    items = records(mo(binary, ["history", "--json", str(copy)], 0), "history")
    h = items[1]
    assert h["folder"] == "ok" and not h["recovered"], h
    assert h["snapshot"]["sequence"] == str(snapshot), (h["snapshot"], snapshot)
    assert h["snapshot"]["later_messages"] == len(rows) == h["log_entries"], (h, rows)
    assert h["snapshot"]["state"] == {"value": {"type": "Main.Gauge", "fields": {
        "armed": False, "count": "0"}}, "complete": True}, h["snapshot"]
    expected = [{"sequence": str(seq), "timer": tool == "expire",
                 "handler_index": 0 if tool == "expire" else fixture.TOOLS.index(tool),
                 "arrival_us": str(arrival), "argument_sizes": [str(len(a)) for a in arguments],
                 "argument_bytes": str(sum(map(len, arguments)))}
                for seq, tool, arrival, arguments in rows]
    assert [{k: e[k] for k in expected[0]} for e in h["log"]] == expected, (h["log"], expected)
    assert all(e["handler"] is None for e in h["log"]), h["log"]
    # One updating bump, arm and disarm, and every tick that changed nothing: indexes 1, 2, 3.
    assert [e["handler_index"] for e in expected if not e["timer"]] == [1, 2, 3], expected
    assert [f["id"] for f in h["failures"]] == [r["id"] for r in live], (h["failures"], live)
    for listed, record in zip(h["failures"], live):
        assert {k: listed[k] for k in record} == record, (listed, record)
        assert listed["build"] == digest and listed["replayable_here"], listed
    text = mo(binary, ["history", str(copy)], 0).stdout.decode()
    assert f"snapshot: as of sequence {snapshot}, {len(rows)} later messages not applied" in text
    for seq, tool, arrival, arguments in rows:
        kind = "timer 0" if tool == "expire" else f"tool {fixture.TOOLS.index(tool)}"
        sizes = "0 arguments" if not arguments else f"1 argument ({len(arguments[0])} bytes)"
        assert f"  {seq}: {kind} (name not recorded in the log), arrival {arrival} us, " \
               f"{sizes}\n" in text, (seq, text)
    assert "note: " + GONE in text and f"failures: {len(live)} retained\n" in text, text
    # The guide finds the recording executable by its digest: sha256sum names this one.
    found = subprocess.run(["sha256sum", str(binary)], capture_output=True, timeout=60, check=True)
    assert found.stdout.split()[0].decode() == digest


def acceptance(binary, folder):
    """The guide's offline how-to, followed step by step, on a service whose read-only tool,
    updating tool and timer each failed."""
    folder.mkdir()
    deployed = folder / "deployed"
    deployed.mkdir()
    file = deployed / "main.mo"
    file.write_text(GAUGE)
    artifacts = {name: folder / f"{name}.moex" for name in ("peek", "bump", "tick", "state")}
    data, ids, live = serve_gauge(binary, folder, artifacts)
    serve_under_reader(binary, folder, data)
    # Copy the whole stopped data folder, then work on the copy.
    copy = folder / "copied"
    shutil.copytree(data, copy)
    gauge_history(binary, copy, live)

    # Each failure replays to reproduced, from the folder and from its fetched artifact.
    recorded = {r["handler"]: r["signature"] for r in live}
    kinds = {"peek": "read-only tool 0, 1 argument", "bump": "updating tool 1, 1 argument",
             "tick": "timer 0, 0 arguments"}
    for handler, failure in ids.items():
        text = mo(binary, replay_args(file, copy, failure), 0).stdout.decode()
        assert text.startswith(f"replay of failure {failure}: {handler} ({kinds[handler]}), "), text
        assert text.endswith("outcome: reproduced\n"), text
        for source in (["--data", str(copy)], ["--export", str(artifacts[handler])]):
            items = records(mo(binary, ["replay", "--json", str(file), "--failure", failure,
                                        *source], 0), "replay")
            original = items[1]
            assert original["outcome"] == "reproduced" and original["handler"] == handler, items
            assert original["run"]["signature"] == recorded[handler], (original, recorded)
            assert original["recorded"]["signature"] == recorded[handler], original

    # Check a fix: copy the program's folder, edit the copy, add --fix COPY/main.mo.
    fixed = folder / "fixed"
    shutil.copytree(deployed, fixed)
    source = GAUGE
    for name in ("peek", "bump", "tick"):
        source = source.replace(f'    stop "{name} failure"\n', "    return " + {
            "peek": "0", "bump": "(Gauge(armed: state.armed, count: 3), 3)",
            "tick": "state"}[name] + "\n")
    (fixed / "main.mo").write_text(source)
    revised = {"peek": "revised: succeeded, reply ok, state unchanged",
               "bump": "revised: succeeded, reply ok, state changed",
               "tick": "revised: succeeded, reply ok, state unchanged"}
    for handler, failure in ids.items():
        text = mo(binary, replay_args(file, copy, failure, "--fix", fixed / "main.mo"),
                  0).stdout.decode()
        assert text.endswith(f"outcome: reproduced\n{revised[handler]}\nverdict: verified\n"), text
    unchanged = folder / "unchanged"
    shutil.copytree(deployed, unchanged)
    text = mo(binary, replay_args(file, copy, ids["tick"], "--fix", unchanged / "main.mo"),
              1).stdout.decode()
    assert text.endswith("verdict: not_fixed\n"), text

    # Query: a program that adds functions, over the exported state and over a failure.
    asking = folder / "asking"
    shutil.copytree(deployed, asking)
    (asking / "main.mo").write_text(GAUGE + GAUGE_QUERIES)
    query = str(asking / "main.mo")
    assert mo(binary, ["query", query, "count", "--export", str(artifacts["state"])],
              0).stdout == b"2\n"
    for source in (["--data", str(copy)], ["--export", str(artifacts["bump"])]):
        result = mo(binary, ["query", query, "inputs", "--failure", ids["bump"], *source], 0)
        assert result.stdout == b"2 7\n", result.stdout

    # A folder it cannot read is refused with its own message.
    if os.geteuid() != 0:
        closed = folder / "closed"
        shutil.copytree(data, closed)
        closed.chmod(0)
        try:
            result = mo(binary, ["history", str(closed)], 1)
            assert result.stderr == f"mo history: cannot read {closed}\n".encode(), result.stderr
        finally:
            closed.chmod(0o700)
    # The original folder was only read.
    assert (data / "service.sqlite").read_bytes() == (copy / "service.sqlite").read_bytes()


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
    "only the first that fails is named.",  # queries: build before handler
    "`CHECK` names are stable; the words after them are not, so branch on `CHECK`.",  # refused
    "a text refusal goes to stderr, and `--json` writes every record to stdout.",  # refused
    "`--recover` on `mo replay` and `mo query` means what it means on `mo history`",  # hot_journal
    "`note` is non-null only when a memory failure did not recur",  # replays: succeeds-memory
    # The steps acceptance follows, in the guide's order.
    "first copy the whole stopped data folder, including any `service.sqlite-journal` beside the "
    "database.",
    "`mo history` shows each record's build digest, and `sha256sum /opt/mo/*/mo` finds the "
    "executable with that digest.",
    "To check a fix, copy the program's folder, edit the copy, and add `--fix COPY/main.mo`.",
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
        history_controls(binary, folder / "controls")
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
        acceptance(binary, folder / "acceptance")
    print(f"{binary}: offline history, replay and query checks passed")


if __name__ == "__main__":
    for executable in sys.argv[1:]:
        check(pathlib.Path(executable).resolve())
