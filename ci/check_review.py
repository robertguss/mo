#!/usr/bin/env python3
"""Approvals and the review setting on a real mo serve, driven through mo review and raw MCP calls:
who may approve, stale and absent items, restarts, activations, conversion of a v0.2.0 folder and
SIGKILL during approvals."""
import http.client
import pathlib
import random
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time

import check_upgrade_live as live
from check_upgrade_live import APPROVAL, ENV, TOKEN, Service, assert_no_secrets, digests, push, write

root = pathlib.Path(__file__).resolve().parent.parent
A = '''module Main
service
  state Int = 0
  tools add
  token from env "TOKEN"
end

fn bump(n: Int) : Int
  promise "bumping raises the number"
    result > n
    allows n: 1, result: 2
    forbids n: 1, result: 1
  end

  examples
    bump(1) == 2
  end

  n + 1
end

fn add(state: Int, _now: Time) : (Int, Int)
  (bump(state), bump(state))
end
'''
B = A.replace("bumping raises the number", "bumping always raises the number")
APPROVE_HEADER = {"Mo-Approval-Authorization": f"Bearer {APPROVAL}"}


def review(binary, service, *args, credential="APPROVAL", status=0):
    result = subprocess.run([binary, "review", "--url", f"http://127.0.0.1:{service.port}/mcp",
                             "--token-env", "TOKEN", "--approval-token-env", credential, *args],
                            capture_output=True, timeout=120, env=ENV)
    assert result.returncode == status, (args, result)
    assert_no_secrets(result.stdout, result.stderr)
    return result.stdout.decode(), result.stderr.decode()


def items(binary, service):
    text, _ = review(binary, service, "--items")
    lines = text.splitlines()
    assert lines[0].startswith("program "), text
    marks = {}
    for line in lines[2:]:
        if line.startswith(("approved ", "unapproved ")):
            mark, digest = line.split()[:2]
            marks[digest] = mark
        else:
            assert line.startswith("  ") or line == lines[1], line
    return lines[0].removeprefix("program "), lines[1], marks


def tool(service, name, headers=None, upgrade=False, **arguments):
    status, body, _ = service.rpc("tools/call", {"name": name, "arguments": arguments},
                                  headers=headers, upgrade=upgrade)
    assert status == 200, (status, body)
    return body["result"]


def history(binary, data):
    result = subprocess.run([binary, "history", str(data)], capture_output=True, timeout=60,
                            env=ENV)
    assert result.returncode == 0, result
    return result.stdout.decode().splitlines()


def refused(result, code, *digests_absent):
    text = result["content"][0]["text"]
    assert result["isError"] and text.startswith(f"error {code}:"), result
    for digest in digests_absent:
        assert digest not in str(result), result


def main_flow(binary, base):
    data = base / "main"
    data.mkdir()
    a, b = write(base, "a", A), write(base, "b", B)
    service = Service(binary, data, a)
    program, setting, marks = items(binary, service)
    assert setting == "review setting: everything; approval version 0", setting
    assert set(marks.values()) == {"unapproved"} and len(marks) == 2, marks
    promise, example = list(marks)

    refused(tool(service, "mo-intent-items"), "E1414", promise, example)
    refused(tool(service, "mo-intent-approve", upgrade=True, program=program, digest=promise),
            "E1414")
    _, error = review(binary, service, "--base", program, "--approve", promise,
                      credential="UPGRADE", status=1)
    assert error.startswith("error E1414: the approval credential is missing or wrong"), error
    assert set(items(binary, service)[2].values()) == {"unapproved"}

    text, _ = review(binary, service, "--base", program, "--approve", promise, "--approve",
                     example)
    assert f"approved {promise}\n" in text and f"approved {example}\n" in text, text
    assert text.endswith("review setting: everything; approval version 2\n"), text
    _, error = review(binary, service, "--base", "0" * 64, "--approve", promise, status=1)
    assert error == f"error E1416: program is not the active program {program}\n", error
    _, error = review(binary, service, "--base", program, "--approve", "f" * 64, status=1)
    assert error == f"error E1416: the active program {program} has no item {'f' * 64}\n", error
    text, _ = review(binary, service, "--setting", "intent")
    assert text == "review setting: intent; approval version 2\n", text
    assert service.stop() == 0

    lines = history(binary, data)
    assert "review setting: intent; approval version 2" in lines, lines
    assert {f"approved {promise} at approval version 1",
            f"approved {example} at approval version 2"} <= set(lines), lines
    decisions = [line for line in lines if line.startswith("decision ")]
    assert [d.split(" at ")[0] for d in decisions] == [
        "decision 1: approval", "decision 2: approval", "decision 3: setting"], decisions
    assert decisions[0].endswith(f"program {program}, setting everything, {promise}"), decisions
    assert decisions[2].endswith(f"program {program}, setting intent"), decisions

    service = Service(binary, data, a)
    assert items(binary, service) == (program, "review setting: intent; approval version 2",
                                      {promise: "approved", example: "approved"})
    base_digest, candidate = digests(binary, a, b)
    assert base_digest == program
    pushed = push(binary, service, b, program)
    assert pushed.returncode == 0, pushed
    after, setting, marks = items(binary, service)
    assert after == candidate and setting == "review setting: intent; approval version 3", setting
    assert marks.pop(example) == "approved" and promise not in marks, marks
    assert list(marks.values()) == ["unapproved"], marks
    assert service.stop() == 0
    lines = history(binary, data)
    assert f"approved {example} at approval version 2" in lines, lines
    assert not any(promise in line for line in lines if line.startswith("approved ")), lines


def conversion(binary, base):
    data = base / "v020"
    shutil.copytree(root / "tests/upgrades/v020", data)
    db = data / "service.sqlite"
    db.chmod(0o600)
    with sqlite3.connect(db) as connection:
        assert connection.execute("PRAGMA user_version").fetchone() == (2,)
        upgrades = connection.execute("SELECT * FROM upgrades").fetchall()
    live.ENV["QUEUE_TOKEN"] = TOKEN
    service = Service(binary, data, data / "main.mo")
    _, setting, marks = items(binary, service)
    assert setting == "review setting: everything; approval version 0", setting
    assert len(marks) == 19 and set(marks.values()) == {"unapproved"}, marks
    assert service.stop() == 0
    with sqlite3.connect(db) as connection:
        assert connection.execute("PRAGMA user_version").fetchone() == (3,)
        assert connection.execute("SELECT * FROM upgrades").fetchall()[:-1] == upgrades
        assert connection.execute("SELECT count(*) FROM approvals").fetchone() == (0,)
        kinds = connection.execute("SELECT kind,setting,detail FROM decisions").fetchall()
    assert kinds == [("conversion", "everything", "")], kinds
    assert any(line.startswith("decision 1: conversion at ") for line in history(binary, data))


def killed_approvals(binary, base):
    data = base / "killed"
    data.mkdir()
    source = (root / "services/task-queue/main.mo").read_text().replace(
        'token from env "QUEUE_TOKEN"', 'token from env "TOKEN"')
    folder = write(base, "queue", source)
    shutil.copy(root / "services/task-queue/job-queue.mo", folder.parent)
    rng = random.Random(1)
    counts = []
    for _ in range(3):
        service = Service(binary, data, folder)
        program, _, marks = items(binary, service)
        pending = [d for d, mark in marks.items() if mark == "unapproved"]
        assert pending, marks

        def approve():
            for digest in pending:
                try:
                    tool(service, "mo-intent-approve", headers=APPROVE_HEADER, program=program,
                         digest=digest)
                except (OSError, http.client.HTTPException, AssertionError):
                    return

        worker = threading.Thread(target=approve)
        worker.start()
        time.sleep(rng.uniform(0.05, 0.4))
        service.kill()
        worker.join()
        with sqlite3.connect(data / "service.sqlite") as connection:
            approved = {row[0].hex(): row[1] for row in
                        connection.execute("SELECT digest,version FROM approvals")}
            decided = [row[0] for row in connection.execute(
                "SELECT detail FROM decisions WHERE kind='approval' ORDER BY seq")]
            (version,) = connection.execute("SELECT version FROM review").fetchone()
        assert sorted(decided) == sorted(approved) and version == len(approved), (
            approved, decided, version)
        assert sorted(approved.values()) == list(range(1, version + 1)), approved
        counts.append(version)
    assert any(0 < count < 41 for count in counts), counts


def main(binary):
    binary = str(pathlib.Path(binary).resolve())
    with tempfile.TemporaryDirectory() as directory:
        base = pathlib.Path(directory)
        main_flow(binary, base)
        conversion(binary, base)
        killed_approvals(binary, base)
    print("review: approvals, the setting, credentials, restarts, activation, v0.2.0 conversion "
          "and kills during approvals passed")


if __name__ == "__main__":
    main(sys.argv[1])
