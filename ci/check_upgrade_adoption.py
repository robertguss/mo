#!/usr/bin/env python3
"""Adoption replay branches, with an explicitly modeled historical image key.

The recorded-key cases below relabel an otherwise real current-build store.
They test identity/replay/adoption control flow, not two compiler implementations
or the ability of a future compiler to reproduce historical language semantics.
The separate v0.1 fixture covers real old-binary replay.
"""
import hashlib
import json
import os
import signal
import pathlib
import shutil
import sqlite3
import sys
import tempfile
from check_upgrades import OLD, TOKEN, run, serve
from check_cli import Served


def checksum(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value.to_bytes(8, "little")


def relabel_frame(record, image, mismatched_state=False):
    assert record[:5] == b"MOST\1" and record[-8:] == checksum(record[:-8])
    assert len(record) == 34 + int.from_bytes(record[22:26], "little")
    body = bytearray(record[:-8])
    body[14:22] = image
    if mismatched_state:
        # A structurally valid log with a different expected post-handler state.
        # Keep outer framing valid so recovery reaches the state checksum check.
        assert body[5] == 1 and len(body) >= 51
        body[26 + 17] ^= 1
    return bytes(body) + checksum(body)


def recorded_rebuild(binary, root, mismatch):
    data = root / ("mismatch" if mismatch else "rebuild")
    data.mkdir()
    source = data / "main.mo"
    source.write_text(OLD.replace("_now: Time) : Int", "_now: Time) : (State, Int)").replace(
        "  state.old_field\n", "  (State(old_field: state.old_field + 1), state.old_field + 1)\n"))
    serve(binary, source, data, 8)
    database = data / "service.sqlite"
    with sqlite3.connect(database) as db:
        image, original_source = db.execute("SELECT image,source FROM program").fetchone()
        historical_image = (int.from_bytes(image, "little") ^ 1).to_bytes(8, "little")
        modeled_build = hashlib.sha256(b"explicit historical-build identity model").digest()
        db.execute("UPDATE program SET image=?,build=?", (historical_image, modeled_build))
        snapshot = db.execute("SELECT record FROM snapshot").fetchone()[0]
        snapshot = relabel_frame(snapshot, historical_image)
        db.execute("UPDATE snapshot SET record=?", (snapshot,))
        logs = db.execute("SELECT seq,record FROM log ORDER BY seq").fetchall()
        assert len(logs) == 1 and logs[0][0] == 1
        for sequence, record in logs:
            db.execute("UPDATE log SET record=? WHERE seq=?",
                       (relabel_frame(record, historical_image, mismatch), sequence))
    before = database.read_bytes()
    if mismatch:
        refused = run(binary, "serve", "--data", data, "--port", "0")
        assert refused.returncode == 1 and b"E1425" in refused.stderr, refused
        assert b"log record 1: read:" in refused.stderr, refused
        assert database.read_bytes() == before, "failed replay changed the store"
        return
    serve(binary, None, data, 9)
    with sqlite3.connect(database) as db:
        ordinal, current_image, build, digest = db.execute(
            "SELECT ordinal,image,build,source FROM program").fetchone()
        assert ordinal == 1 and current_image == image and build != modeled_build
        assert digest == original_source, "rebuild changed original source identity"
        assert db.execute("SELECT record FROM previous_snapshots WHERE ordinal=1").fetchone()[0] == snapshot
        report = json.loads(db.execute("SELECT report FROM upgrades WHERE ordinal=1").fetchone()[0])
        assert report["class"] == "adoption" and "1 log records" in report["message"], report
    serve(binary, None, data, 10)


def same_image_build_adoption(binary, root):
    # Only the historical build digest is modeled. Image, schema, source and
    # snapshot are real current-build outputs and remain byte-for-byte intact.
    data = root / "same-image-build"
    data.mkdir()
    source = data / "main.mo"
    text = OLD.replace("  tools read\n", "  tools read, broken\n") + (
        'fn broken(_state: State, _now: Time) : Int\n  stop "pin this build"\nend\n')
    source.write_text(text)
    serve(binary, source, data, 7)
    database = data / "service.sqlite"
    historical_build = hashlib.sha256(b"modeled older runtime with identical image").digest()
    with sqlite3.connect(database) as db:
        original = db.execute("SELECT image,build,source,bundle,context FROM program").fetchone()
        snapshot = db.execute("SELECT record FROM snapshot").fetchone()[0]
        assert original[1] != historical_build
        db.execute("UPDATE program SET build=?", (historical_build,))
    serve(binary, None, data, 7)
    with sqlite3.connect(database) as db:
        active = db.execute("SELECT image,build,source,bundle,context,ordinal FROM program").fetchone()
        assert active[:5] == original and active[5] == 1, active
        assert db.execute("SELECT build FROM previous_programs WHERE ordinal=1").fetchone()[0] == historical_build
        assert db.execute("SELECT record FROM previous_snapshots WHERE ordinal=1").fetchone()[0] == snapshot
    served = Served(binary, source, data, {**os.environ, "TOKEN": TOKEN}, root)
    try:
        status, body = served.post(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
            "params": {"name": "broken", "arguments": {}}}), TOKEN)
        assert status == 200 and json.loads(body)["result"]["isError"], body
        errors = served.finish(0, signal.SIGTERM)
        assert b"pin this build" in errors, errors
    finally:
        served.close()
    history = run(binary, "history", data, "--json")
    assert history.returncode == 0, history
    row = next(json.loads(line) for line in history.stdout.splitlines()
               if json.loads(line).get("record") == "history")
    assert row["failures"] and all(f["build"] == original[1].hex() for f in row["failures"]), row
    for offset in range(1, 4):
        source.write_text(text.replace("  state.old_field\n", f"  state.old_field + {offset}\n"))
        serve(binary, source, data, 7 + offset, "--upgrade")
    with sqlite3.connect(database) as db:
        pinned = db.execute("SELECT image,build,source,bundle,context FROM previous_programs "
                            "WHERE ordinal=2").fetchone()
        assert pinned == original, "the failure's exact program and context were evicted"
        assert db.execute("SELECT count(*) FROM previous_programs").fetchone()[0] == 3
        assert db.execute("SELECT count(*) FROM previous_programs WHERE ordinal=1").fetchone()[0] == 0


def legacy_empty_tail(binary, root):
    fixture = pathlib.Path(__file__).resolve().parent.parent / "tests/upgrades/v010"
    data = root / "empty-legacy"
    shutil.copytree(fixture, data)
    database = data / "service.sqlite"
    with sqlite3.connect(database) as db:
        # Derive the real v0.1 initial snapshot, before either recorded message.
        snapshot = db.execute("SELECT record FROM snapshot").fetchone()[0]
        assert int.from_bytes(snapshot[6:14], "little") == 0
        db.execute("DELETE FROM log")
        db.execute("UPDATE head SET seq=0")
    source = data / "main.mo"
    source.write_text("# explicit empty-tail adoption\n" + source.read_text())
    before = database.read_bytes()
    refused = run(binary, "serve", source, "--data", data, "--port", "0")
    assert refused.returncode == 1 and b"E1430" in refused.stderr, refused
    assert database.read_bytes() == before
    serve(binary, source, data, lambda value: value["jobs"] == [], "--adopt", tool="list",
          arguments={"filter": {"case": "AllJobs"}, "after": "0", "limit": "10"})
    with sqlite3.connect(database) as db:
        assert db.execute("SELECT record FROM previous_snapshots WHERE ordinal=1").fetchone()[0] == snapshot
        report = json.loads(db.execute("SELECT report FROM upgrades WHERE ordinal=1").fetchone()[0])
        assert "0 log records" in report["message"] and "tail is empty" in report["message"], report


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="mo-adoption-") as tmp:
        root = pathlib.Path(tmp)
        recorded_rebuild(binary, root, False)
        recorded_rebuild(binary, root, True)
        legacy_empty_tail(binary, root)
        same_image_build_adoption(binary, root)
    print("Recorded rebuild replay and explicit empty-tail adoption passed")


if __name__ == "__main__":
    main()
