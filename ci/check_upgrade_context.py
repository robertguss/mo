#!/usr/bin/env python3
"""Read-only prechecks use the active program's retained compilation context."""
import json
import os
import pathlib
import signal
import sqlite3
import sys
import tempfile
from check_cli import Served
from check_upgrades import OLD, NEW, TOKEN, run, serve


def checked(binary, candidate, data, *flags):
    database = data / "service.sqlite"
    before = database.read_bytes()
    files = sorted(p.name for p in data.iterdir())
    result = run(binary, "check", candidate, "--upgrade-from", data, "--json", *flags)
    assert result.returncode == 0 and not result.stderr, result
    report = json.loads(result.stdout)
    assert report["status"] == "accepted", report
    assert database.read_bytes() == before and sorted(p.name for p in data.iterdir()) == files
    return report


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="mo-check-context-") as tmp:
        root = pathlib.Path(tmp)
        data = root / "data"
        data.mkdir()
        source = root / "main.mo"
        source.write_text(OLD)
        missing = run(binary, "check", source, "--upgrade-from", data, "--json")
        assert missing.returncode == 1 and json.loads(missing.stdout)["code"] == "E1425", missing
        assert list(data.iterdir()) == [], "checking an empty folder created a database"
        serve(binary, source, data, 7)
        source.write_text(NEW)
        serve(binary, source, data, 17, "--upgrade", "--migrate", "migrate")
        for offset in range(1, 4):
            source.write_text(NEW.replace("state.new_field\n", f"state.new_field + {offset}\n"))
            checked(binary, source, data)
            serve(binary, source, data, 17 + offset, "--upgrade")
        with sqlite3.connect(data / "service.sqlite") as db:
            assert [r[0] for r in db.execute("SELECT ordinal FROM previous_programs ORDER BY ordinal")] == [3, 4]
            digest = db.execute("SELECT source FROM program").fetchone()[0].hex()
        report = checked(binary, source, data)
        assert report["base_source"] == digest and report["candidate_source"] == digest
        # No migration is run by check; the next initial value is deliberately unrelated.
        source.write_text(NEW.replace("new_field", "final_field").replace(
            "old.old_field + 10", "old.new_field + 100").replace("fn migrate(", "fn migrate_again("))
        checked(binary, source, data, "--migrate", "migrate_again")
        serve(binary, source, data, 117, "--upgrade", "--migrate", "migrate_again")
        serve(binary, None, data, 117)
        checked(binary, source, data)
        # Read-only readers do not wait for the live exclusive owner or recover its database.
        served = Served(binary, source, data, {**os.environ, "TOKEN": TOKEN}, root)
        try:
            before = (data / "service.sqlite").read_bytes()
            refused = run(binary, "check", source, "--upgrade-from", data, "--json")
            assert refused.returncode == 1 and json.loads(refused.stdout)["code"] == "E1415", refused
            assert (data / "service.sqlite").read_bytes() == before
            assert served.finish(0, signal.SIGTERM)
        finally:
            served.close()
        database = data / "service.sqlite"
        with sqlite3.connect(database) as db:
            db.execute("UPDATE program SET context_digest=zeroblob(32)")
        before = database.read_bytes()
        refused = run(binary, "check", source, "--upgrade-from", data, "--json")
        assert refused.returncode == 1 and json.loads(refused.stdout)["code"] == "E1425", refused
        assert database.read_bytes() == before, "invalid context precheck changed the store"
    with tempfile.TemporaryDirectory(prefix="mo-upgrade-tail-") as tmp:
        root = pathlib.Path(tmp)
        data = root / "data"
        data.mkdir()
        source = root / "main.mo"
        source.write_text(OLD.replace("_now: Time) : Int", "_now: Time) : (State, Int)").replace(
            "  state.old_field\n",
            "  (State(old_field: state.old_field + 1), state.old_field + 1)\n"))
        serve(binary, source, data, 8)
        with sqlite3.connect(data / "service.sqlite") as db:
            assert db.execute("SELECT seq FROM log").fetchall() == [(1,)]
        source.write_text(NEW)
        checked(binary, source, data, "--migrate", "migrate")
        serve(binary, source, data, 18, "--upgrade", "--migrate", "migrate")
        with sqlite3.connect(data / "service.sqlite") as db:
            assert db.execute("SELECT head FROM upgrades").fetchall() == [(1,)]
            assert db.execute("SELECT count(*) FROM log").fetchone()[0] == 0
        serve(binary, None, data, 18)
    print("Recorded-context prechecks preserve the store across retention and migrations")


if __name__ == "__main__":
    main()
