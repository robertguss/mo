#!/usr/bin/env python3
"""Empty-tail schema fallback and refusal to invent unreplayable history."""
import hashlib
import pathlib
import sqlite3
import sys
import tempfile
from check_upgrades import OLD, NEW, run, serve


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    for schema_only, tail in [(False, False), (True, False), (False, True)]:
        with tempfile.TemporaryDirectory(prefix="mo-upgrade-fallback-") as tmp:
            root = pathlib.Path(tmp)
            data = root / "data"
            data.mkdir()
            source = root / "main.mo"
            old = OLD
            if tail:
                old = old.replace("_now: Time) : Int", "_now: Time) : (State, Int)").replace(
                    "  state.old_field\n", "  (state, state.old_field)\n")
            source.write_text(old)
            serve(binary, source, data, 7)
            with sqlite3.connect(data / "service.sqlite") as con:
                bundle, context = con.execute("select bundle, context from program").fetchone()
                broken = bundle.replace(b"state.old_field", b"state.bad_field")
                assert broken != bundle
                # Simulate a later compiler rejecting a previously valid body.
                con.execute("update program set bundle=?,source=?",
                            (broken, hashlib.sha256(broken).digest()))
                if schema_only:
                    start = context.index(b"MOCS\1")
                    broken_context = context[:start] + context[start:].replace(
                        b"old_field: Int", b"bad_field: Int")
                    assert broken_context != context
                    con.execute("update program set context=?,context_digest=?",
                                (broken_context, hashlib.sha256(broken_context).digest()))
                con.commit()
                before = list(con.iterdump())
                log_count = con.execute("select count(*) from log").fetchone()[0]
                assert bool(log_count) == tail
            refusal = run(binary, "serve", "--data", data, "--port", "0")
            assert refusal.returncode == 1 and b"E1429" in refusal.stderr, refusal
            source.write_text(NEW)
            if tail:
                result = run(binary, "serve", source, "--data", data, "--port", "0",
                             "--upgrade", "--migrate", "migrate")
                assert result.returncode == 1 and b"E1429" in result.stderr, result
                with sqlite3.connect(data / "service.sqlite") as con:
                    assert list(con.iterdump()) == before
            else:
                serve(binary, source, data, 17, "--upgrade", "--migrate", "migrate")
                serve(binary, None, data, 17)
    print("upgrade fallback: pass")


if __name__ == "__main__":
    main()
