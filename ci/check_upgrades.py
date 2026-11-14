#!/usr/bin/env python3
"""Typed admission and checked restart through the real persistent service."""
import hashlib
import http.client
import json
import os
import pathlib
import select
import signal
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

OLD = '''module Main
struct State
  old_field: Int
end
service
  state State = State(old_field: 7)
  tools read
  token from env "TOKEN"
end
fn read(state: State, _now: Time) : Int
  state.old_field
end
'''
NEW = '''module Main
use UpgradeOld
struct State
  new_field: Int
end
service
  state State = State(new_field: 999)
  tools read
  token from env "TOKEN"
end
fn read(state: State, _now: Time) : Int
  state.new_field
end
fn migrate(old: UpgradeOld.State) : State
  State(new_field: old.old_field + 10)
end
'''
TOKEN = "UpgradeRegressionToken0123456789"


def run(binary, *args):
    return subprocess.run([str(binary), *map(str, args)], capture_output=True,
                          timeout=60, env={**os.environ, "TOKEN": TOKEN, "QUEUE_TOKEN": TOKEN})


def serve(binary, source, data, expected, *flags, tool="read", arguments=None):
    args = [str(binary), "serve", *([str(source)] if source else []),
            "--data", str(data), "--port", "0", *flags]
    with tempfile.TemporaryFile() as errors:
        process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=errors,
                                   env={**os.environ, "TOKEN": TOKEN, "QUEUE_TOKEN": TOKEN}, bufsize=0)
        try:
            deadline = time.monotonic() + 45
            port = None
            line = b""
            while port is None:
                remaining = deadline - time.monotonic()
                assert remaining > 0, "service did not start"
                assert select.select([process.stdout], [], [], remaining)[0], "startup timeout"
                byte = process.stdout.read(1)
                if not byte:
                    errors.seek(0)
                    raise AssertionError((args, process.poll(), errors.read()))
                line += byte
                if byte == b"\n":
                    prefix = b"mo serve: listening on http://127.0.0.1:"
                    if line.startswith(prefix):
                        assert line.endswith(b"/mcp\n"), line
                        port = int(line[len(prefix):-5])
                    line = b""
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
            try:
                body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                   "params": {"name": tool, "arguments": arguments or {}}})
                connection.request("POST", "/mcp", body, {
                    "Authorization": "Bearer " + TOKEN, "Content-Type": "application/json",
                    "MCP-Protocol-Version": "2025-11-25"})
                response = connection.getresponse()
                result = json.loads(response.read())
                assert response.status == 200 and not result["result"]["isError"], result
                value = result["result"]["structuredContent"]["value"]
                assert expected(value) if callable(expected) else value == str(expected), result
            finally:
                connection.close()
            process.send_signal(signal.SIGTERM)
            process.communicate(timeout=30)
            errors.seek(0)
            stderr = errors.read()
            assert process.returncode == 0, stderr
            assert TOKEN.encode() not in stderr, stderr
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            process.stdout.close()


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="mo-upgrades-") as tmp:
        root = pathlib.Path(tmp)
        data = root / "data"
        data.mkdir()
        # Separate directories keep entry names and module identity unchanged.
        (root / "old").mkdir()
        (root / "new").mkdir()
        old, new = root / "old/main.mo", root / "new/main.mo"
        old.write_text(OLD)
        new.write_text(NEW)
        checked = run(binary, "check", new, "--upgrade-from", old, "--migrate", "migrate", "--json")
        assert checked.returncode == 0 and not checked.stderr, checked
        compared = json.loads(checked.stdout)
        assert compared["status"] == "accepted", checked.stdout
        comparison_fields = {"old_schema", "new_schema", "implementation_changed", "differences"}
        work_fields = {"vm_work", "native_work", "heap_peak", "encoded_bytes"}
        assert comparison_fields | work_fields <= compared.keys(), compared
        missing = run(binary, "check", new, "--upgrade-from", old, "--json")
        assert missing.returncode == 1, missing
        assert json.loads(missing.stdout)["code"] == "E1421", missing.stdout
        damaged = root / "damaged-folder"
        damaged.mkdir()
        (damaged / "service.sqlite").write_bytes(b"not a SQLite database")
        unavailable = run(binary, "serve", old, "--data", damaged, "--port", "0")
        assert unavailable.returncode == 1 and b"E1409" in unavailable.stderr, unavailable
        serve(binary, old, data, 7)
        database = data / "service.sqlite"
        before = database.read_bytes()
        mismatch = run(binary, "serve", new, "--data", data, "--port", "0")
        assert mismatch.returncode == 1 and b"E1430" in mismatch.stderr, mismatch
        assert database.read_bytes() == before, "source refusal mutated the folder"
        serve(binary, new, data, 17, "--upgrade", "--migrate", "migrate")
        serve(binary, None, data, 17)
        with sqlite3.connect(database) as db:
            context = db.execute("SELECT context FROM program").fetchone()[0]
        for offset in range(1, 4):
            new.write_text(NEW.replace("state.new_field\n", f"state.new_field + {offset}\n"))
            serve(binary, new, data, 17 + offset, "--upgrade")
            serve(binary, None, data, 17 + offset)
        history = run(binary, "history", data, "--json")
        assert history.returncode == 0, history
        records = [json.loads(line) for line in history.stdout.splitlines()]
        recorded = next(row for row in records if row.get("record") == "history")
        assert recorded["program"]["ordinal"] == "4", recorded
        assert [row["ordinal"] for row in recorded["upgrades"]] == ["1", "2", "3", "4"]
        assert [row["ordinal"] for row in recorded["previous_programs"]] == ["3", "4"]
        with sqlite3.connect(database) as db:
            ordinal, source, bundle, digest, active = db.execute(
                "SELECT ordinal,source,bundle,context_digest,context FROM program").fetchone()
            assert ordinal == 4 and source == hashlib.sha256(bundle).digest()
            assert active == context and digest == hashlib.sha256(active).digest()
            retained = db.execute("SELECT ordinal,context_digest,context FROM previous_programs ORDER BY ordinal").fetchall()
            assert [row[0] for row in retained] == [3, 4], retained
            assert all(row[2] == context and row[1] == hashlib.sha256(context).digest()
                       for row in retained)
            assert db.execute("SELECT count(*) FROM upgrades").fetchone()[0] == 4
            db.execute("UPDATE program SET context_digest=zeroblob(32)")
        before = database.read_bytes()
        corrupt = run(binary, "serve", "--data", data, "--port", "0")
        assert corrupt.returncode == 1 and b"E1425" in corrupt.stderr, corrupt
        assert database.read_bytes() == before, "context refusal mutated the folder"
        refusing = root / "refusal-data"
        refusing.mkdir()
        serve(binary, old, refusing, 7)
        refusal_db = refusing / "service.sqlite"
        traps = (
            ("E1423", NEW.replace("old.old_field + 10", "old.old_field / (old.old_field - old.old_field)")),
            ("E1424", NEW.replace("  State(new_field: old.old_field + 10)",
                "  var n = 0\n  while n < 4000000\n    n += 1\n  end\n  State(new_field: old.old_field + n)")),
        )
        for code, text in traps:
            new.write_text(text)
            before = refusal_db.read_bytes()
            result = run(binary, "serve", new, "--data", refusing, "--port", "0", "--upgrade", "--migrate", "migrate")
            assert result.returncode == 1 and code.encode() in result.stderr, result
            assert refusal_db.read_bytes() == before, "migration refusal mutated persistent state"
            serve(binary, None, refusing, 7)
        heavy = root / "heavy-migration"
        heavy.mkdir()
        serve(binary, old, heavy, 7)
        new.write_text(NEW.replace("  State(new_field: old.old_field + 10)",
            "  var n = 0\n  while n < 200000\n    n += 1\n  end\n  State(new_field: old.old_field + n)"))
        serve(binary, new, heavy, 200007, "--upgrade", "--migrate", "migrate")
        with sqlite3.connect(heavy / "service.sqlite") as db:
            report = json.loads(db.execute("SELECT report FROM upgrades WHERE ordinal=1").fetchone()[0])
            work = int(report["vm_work"]) + int(report["native_work"])
            assert 1048576 < work <= 16777216, report
        serve(binary, None, heavy, 200007)
        new.write_text(NEW.replace("  tools read\n", "  tools read\n  every 0.seconds tick\n") +
                       "fn tick(state: State, _now: Time) : State\n  state\nend\n")
        before = refusal_db.read_bytes()
        timer = run(binary, "serve", new, "--data", refusing, "--port", "0", "--upgrade", "--migrate", "migrate")
        assert timer.returncode == 1 and b"E1419" in timer.stderr, timer
        assert refusal_db.read_bytes() == before, "invalid timer mutated the folder"
        new.write_text(NEW.replace("old.old_field + 10", "missing()"))
        invalid = run(binary, "check", new, "--upgrade-from", old, "--migrate", "migrate", "--json")
        invalid_report = json.loads(invalid.stdout)
        assert invalid.returncode == 1 and invalid_report["code"] == "E1419", invalid
        assert comparison_fields.isdisjoint(invalid_report), invalid_report
        assert work_fields <= invalid_report.keys(), invalid_report
        new.write_text(OLD + "# padding\n" * 110000)
        oversized = run(binary, "check", new, "--upgrade-from", old)
        assert oversized.returncode == 1 and b"E1417" in oversized.stderr, oversized
        # A capability/generic/result mismatch uses the signature code E1422.
        for text in (
            NEW.replace("fn migrate(old: UpgradeOld.State)", "fn migrate(old: UpgradeOld.State, _io: Console)"),
            NEW.replace("fn migrate(old: UpgradeOld.State)", "fn migrate(old: UpgradeOld.State, _extra: T)"),
            NEW.replace("fn migrate(old: UpgradeOld.State) : State\n  State(new_field: old.old_field + 10)",
                        "fn migrate(old: UpgradeOld.State) : Int\n  old.old_field"),
        ):
            new.write_text(text)
            result = run(binary, "check", new, "--upgrade-from", old, "--migrate", "migrate", "--json")
            assert result.returncode == 1 and json.loads(result.stdout)["code"] == "E1422", result
        new.write_text(NEW.replace('"TOKEN"', '"OTHER_TOKEN"'))
        result = run(binary, "check", new, "--upgrade-from", old, "--migrate", "migrate", "--json")
        assert result.returncode == 1 and json.loads(result.stdout)["code"] == "E1426", result
        generic = root / "generic-data"
        generic.mkdir()
        fixtures = pathlib.Path(__file__).resolve().parent.parent / "tests/upgrades/generic"
        serve(binary, fixtures / "old/main.mo", generic, 7)
        serve(binary, fixtures / "new/main.mo", generic, 7, "--upgrade", "--migrate", "migrate")
        serve(binary, None, generic, 7)
        legacy = root / "legacy"
        fixture = pathlib.Path(__file__).resolve().parent.parent / "tests/upgrades/v010"
        shutil.copytree(fixture, legacy)
        legacy_db = legacy / "service.sqlite"
        with sqlite3.connect(legacy_db) as db:
            original_snapshot = db.execute("SELECT record FROM snapshot").fetchone()[0]
            assert db.execute("PRAGMA user_version").fetchone()[0] == 1
            assert db.execute("SELECT count(*) FROM log").fetchone()[0] == 2
        # Shift debug locations without changing the handlers' results, forcing an image mismatch.
        legacy_source = legacy / "main.mo"
        legacy_source.write_text("# explicitly adopted revision\n" + legacy_source.read_text())
        before = legacy_db.read_bytes()
        rejected = run(binary, "serve", legacy_source, "--data", legacy, "--port", "0")
        assert rejected.returncode == 1, rejected
        assert legacy_db.read_bytes() == before, "unapproved legacy adoption changed the folder"
        def preserved(value):
            return len(value["jobs"]) == 1 and value["jobs"][0]["payload"] == "legacy-preserved"
        serve(binary, legacy_source, legacy, preserved, "--adopt", tool="list",
              arguments={"filter": {"case": "AllJobs"}, "after": "0", "limit": "10"})
        with sqlite3.connect(legacy_db) as db:
            assert db.execute("PRAGMA user_version").fetchone()[0] == 3
            assert db.execute("SELECT ordinal FROM program").fetchone()[0] == 1
            assert db.execute("SELECT record FROM previous_snapshots WHERE ordinal=1").fetchone()[0] == original_snapshot
            assert db.execute("SELECT old_source FROM upgrades WHERE ordinal=1").fetchone()[0] == bytes(32)
            report = db.execute("SELECT report FROM upgrades WHERE ordinal=1").fetchone()[0]
            assert "2 log records" in report, report
        serve(binary, None, legacy, preserved, tool="list",
              arguments={"filter": {"case": "AllJobs"}, "after": "0", "limit": "10"})
    print("Typed upgrade checks, migrated values, retained contexts and restart refusals passed")


if __name__ == "__main__":
    main()
