#!/usr/bin/env python3
"""Authenticated live views against a real persistent service, without author inspection tools."""
import base64
import hashlib
import json
import os
import pathlib
import signal
import struct
import sys
import tempfile
import time

from cli_common import SERVE_OPERATOR_NAMES as NAMES, SERVE_UPGRADE_NAMES, SERVE_REVIEW_NAMES, Served, serve_events, serve_supervised

TOKEN = "OperatorViewCanary"
VIEW_BYTES = 6144
STOPPED = b"mo serve: stop: a signal asked the service to stop\n"
SOURCE = '''module Main
struct Secret
  answer: Int
  text: Text
  bytes: Bytes
  big: BigInt
  decimal: Decimal
  float: Float
  time: Time
  range: Range
  tuple: (Bool, Char)
  choice: Option(Int)
  map: Map(Int, Text)
  set: Set(Int)
  items: List(Int)
  note: Text
  large: Text
  long_map: Map(Text, Text)
end
service
  state Secret = initial()
  tools mo_state
  every 1.hour tick
  token from env "OPERATOR_TOKEN"
end
fn initial() : Secret
  Secret(answer: 42, text: "private é", bytes: "abc".bytes,
    big: -123456789012345678901234567890, decimal: -1.250000000000000001,
    float: 1.5, time: Time.parse("2026-11-05T01:02:03.123456Z") or stop "fixture",
    range: 3..7, tuple: (true, 'λ'), choice: Some(19), map: {3: "three"},
    set: [5, 2].to_set, items: [7, 9], note: "n".repeat(2000), large: "x".repeat(100000),
    long_map: {"x".repeat(7000): "three", "y": "four"})
end
fn mo_state(_state: Secret, _now: Time) : Int
  777
end
fn tick(state: Secret, _now: Time) : Secret
  state
end
'''
WIDE = '''module Main
struct Wide
  ints: List(Int)
  texts: List(Text)
  empties: List(Text)
  answer: Int
end
service
  state Wide = Wide(ints: (1..20001).to_list, texts: (1..10001).to_list.map(fn(i) i.to_text end),
    empties: (1..6001).to_list.map(fn(_i) "" end), answer: 42)
  tools read
  token from env "OPERATOR_TOKEN"
end
fn read(state: Wide, _now: Time) : Int
  state.answer
end
'''


FAILING = '''module Main
service
  state Text = "x".repeat(20000)
  tools broken, add
  token from env "OPERATOR_TOKEN"
end
fn broken(_state: Text, _now: Time, _n: Int) : Bool
  stop "operator failure"
end
fn add(state: Text, _now: Time) : (Text, Bool)
  ("#{state}x", true)
end
'''


def client(served):
    assert served.port, served.errors()

    def rpc(method, params, token=TOKEN):
        time.sleep(0.055)
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params})
        status, raw = served.post(body, token)
        assert TOKEN.encode() not in raw
        return status, json.loads(raw) if raw else None

    def call(name, **arguments):
        status, body = rpc("tools/call", {"name": name, "arguments": arguments})
        assert status == 200 and not body["result"]["isError"], (name, body)
        value = body["result"]["structuredContent"]["value"]
        if name == "mo-state" and "state" in value:
            rendered = json.dumps(value["state"], ensure_ascii=False, separators=(",", ":"))
            assert len(rendered.encode()) <= VIEW_BYTES, (arguments, len(rendered))
        return value

    return rpc, call


def serving(binary, source, check):
    with tempfile.TemporaryDirectory(prefix="mo-operator-") as directory:
        folder = pathlib.Path(directory)
        path = folder / "main.mo"
        path.write_text(source, encoding="utf-8")
        data = folder / "data"
        data.mkdir()
        served = Served(binary, path, data, {**os.environ, "OPERATOR_TOKEN": TOKEN}, folder)
        try:
            check(*client(served))
            assert served.finish(0, signal.SIGTERM) == STOPPED
        finally:
            served.close()


def exported(call, page, check=None):
    """Read every page after `page`, checking each page's metadata; return the artifact bytes."""
    from check_export import fnv

    first, data = page, bytearray()
    while True:
        for key in ["export", "kind", "failure", "version", "sequence", "encoded_sequence",
                    "total", "pages"]:
            assert page[key] == first[key], (key, first, page)
        raw = base64.b64decode(page["base64"], validate=True)
        assert page["offset"] == len(data) and len(raw) == page["length"], page
        assert fnv(raw) == page["checksum"], page
        data.extend(raw)
        if page["next"] is None:
            break
        assert page["digest"] is None, page
        expected = {"kind": first["kind"], "export": first["export"], "offset": len(data)}
        if first["kind"] == "failure":
            expected["failure"] = first["failure"]
        assert page["next"] == expected, page
        if check is not None:
            check(page)
        page = call("mo-export", **page["next"])
    assert len(data) == first["total"] and first["pages"] == -(-len(data) // 8192)
    assert hashlib.sha256(data[:-32]).digest() == data[-32:] and data[-32:].hex() == page["digest"]
    assert data[:6] == b"MOEX\1" + (b"\2" if first["kind"] == "failure" else b"\1")
    assert data[6:22].hex() == first["export"]
    return bytes(data)


def body(artifact):
    at = 66 + struct.unpack_from("<I", artifact, 62)[0]
    size = struct.unpack_from("<I", artifact, at + 60)[0]
    assert len(artifact) == at + 64 + size + 32, (len(artifact), size)
    return artifact[at + 64:-32]


def failure_records(binary):
    with tempfile.TemporaryDirectory(prefix="mo-operator-failures-") as directory:
        folder = pathlib.Path(directory)
        path = folder / "main.mo"
        path.write_text(FAILING, encoding="utf-8")
        data = folder / "data"
        data.mkdir()
        env = {**os.environ, "OPERATOR_TOKEN": TOKEN}
        served = Served(binary, path, data, env, folder)
        try:
            rpc, call = client(served)

            def broken(n):
                status, reply = rpc("tools/call", {"name": "broken", "arguments": {"_n": str(n)}})
                assert status == 200 and reply["result"]["isError"] is True, reply
                error = reply["result"]["structuredContent"]["error"]
                assert error["code"] == "handler_failed" and error["effect"] == "none", reply

            def refused(**arguments):
                status, reply = rpc("tools/call", {"name": "mo-export", "arguments": arguments})
                assert status == 200 and reply["result"]["isError"] is True, reply
                return reply["result"]["structuredContent"]["value"]

            opened = call("mo-overview")
            assert opened["retained_failures"] == 0 and opened["failure_capture_available"] is True
            assert opened["evicted_kinds_since_open"] == "0", opened
            broken(7)
            broken(8)
            listed = call("mo-failures")
            assert listed["truncated"] == 0 and listed["next"] is None, listed
            assert listed["unavailable"] == [] and listed["unavailable_omitted"] == 0, listed
            records = listed["records"]
            assert [record["id"] for record in records] == ["1", "2"], listed
            for record in records:
                assert record["handler"] == "broken" and record["kind"] == records[0]["kind"]
                assert record["kind"].startswith("broken:0:stop:"), record
                signature = record["signature"]
                assert signature["version"] == 1 and signature["class"] == "defect", record
                assert signature["reason"] == "stop" and signature["limit"] == "", record
                assert record["state_bytes"] == records[0]["state_bytes"], records
            assert 20000 < int(records[0]["state_bytes"]) < 20100, records
            assert int(records[0]["arrival"]) < int(records[1]["arrival"]), records
            assert call("mo-failures", after=1)["records"] == records[1:]
            assert call("mo-failures", after=2)["records"] == []
            overview = call("mo-overview")
            assert overview["retained_failures"] == 2 and overview["failures"] == 2, overview
            events = call("mo-supervisor")["items"]
            assert [(e["kind"], e["handler"], e["record"], e["record_unavailable"])
                    for e in events] == [("failure", "broken", "2", None),
                                         ("failure", "broken", "1", None)], events

            assert refused(kind="failure", failure=3) == {"error": "failure_unavailable",
                                                          "state_effect": "none"}
            first = call("mo-export", kind="failure", failure=1)
            assert first["kind"] == "failure" and first["failure"] == 1, first
            assert first["offset"] == 0 and first["length"] == 8192 and first["pages"] == 3, first
            assert first["sequence"] == first["encoded_sequence"] == "0", first
            assert call("mo-memory")["pinned_export_bytes"] >= first["total"]

            def switch(page):
                assert refused(**{**page["next"], "failure": 2}) == {
                    "error": "export_replaced", "state_effect": "none"}

            artifact = exported(call, first, switch)
            assert call("mo-memory")["pinned_export_bytes"] == 0
            pair = body(artifact)
            assert b"x" * 20000 in pair and b"broken" in pair
            assert struct.unpack_from("<Q", artifact, 22)[0] == int(opened["program_identity"])

            for _ in range(100):
                call("add")
            snapshot = call("mo-overview")
            assert snapshot["recovery_sequence"] == snapshot["snapshot_sequence"] == "100", snapshot
            assert call("mo-failures")["records"] == records
            for n in range(13):
                broken(n)
            errors = served.finish(69)
        finally:
            served.close()
        assert serve_events(errors) == serve_supervised(b"broken"), errors
        lines = [line for line in errors.split(b"\n") if line.startswith(b"  record")]
        assert lines == [b"  record: %d" % i for i in range(1, 16)], lines

        served = Served(binary, path, data, env, folder)
        try:
            _, call = client(served)
            kept = call("mo-failures")["records"]
            assert [record["id"] for record in kept] == ["1", "15"] and kept[0] == records[0], kept
            assert int(kept[1]["state_bytes"]) == int(records[0]["state_bytes"]) + 100, kept
            assert kept[1]["kind"] == records[0]["kind"], kept
            reopened = call("mo-overview")
            assert reopened["retained_failures"] == 2 and reopened["recovery_sequence"] == "100"
            latest = exported(call, call("mo-export", kind="failure", failure=15))
            assert b"x" * 20100 in body(latest)
            assert served.finish(0, signal.SIGTERM) == STOPPED
        finally:
            served.close()


def private_state(rpc, call):
    status, body = rpc("tools/list", {})
    tools = body["result"]["tools"]
    assert status == 200 and [tool["name"] for tool in tools] == ["mo_state", *NAMES, *SERVE_UPGRADE_NAMES, *SERVE_REVIEW_NAMES]
    assert "annotations" not in tools[0]
    for tool in tools[1:]:
        expected = {"readOnlyHint": True, "idempotentHint": True}
        if tool["name"] in ("mo-export", "mo-intent-items"):
            expected["openWorldHint"] = False
        if tool["name"] in SERVE_REVIEW_NAMES:
            assert "Mo-Approval-Authorization: Bearer VALUE" in tool["description"], tool
        if tool["name"] in SERVE_REVIEW_NAMES[1:]:
            expected = {"readOnlyHint": False, "idempotentHint": False, "openWorldHint": False,
                        "destructiveHint": False}
        if tool["name"] in SERVE_UPGRADE_NAMES:
            assert "Mo-Upgrade-Authorization: Bearer VALUE" in tool["description"], tool
            assert len(tool["description"].encode()) <= 512
            expected = {"readOnlyHint": False, "idempotentHint": False, "openWorldHint": False,
                        "destructiveHint": tool["name"] == "mo-upgrade-commit"}
        assert tool["annotations"] == expected, tool
    before = call("mo-overview")
    assert before["runtime_identity"] and len(before["runtime_identity"]) == 64
    whole = call("mo-state")
    fields = whole["state"]["fields"]
    assert not whole["complete"] and fields["answer"] == "42", whole
    assert "case" not in whole["state"] and fields["note"] == "n" * 2000, whole
    assert fields["large"] == {"truncated_bytes": 100000}, whole
    expected = {"answer": "42", "text": "private é", "bytes": {"base64": "YWJj"},
                "big": "-123456789012345678901234567890", "decimal": "-1.250000000000000001",
                "float": 1.5, "time": "2026-11-05T01:02:03.123456Z",
                "range": {"start": "3", "end": "7"}, "tuple": [True, "λ"],
                "choice": {"type": "Option(Int)", "case": "Some", "value": "19"},
                "map": [["3", "three"]], "set": ["5", "2"], "items": ["7", "9"]}
    for key, value in expected.items():
        result = call("mo-state", path=[key])
        assert result["complete"] and result["state"] == value, (key, result)
    assert call("mo-state", path=["items", 1])["state"] == "9"
    assert call("mo-state", path=["map", 0])["state"] == ["3", "three"]
    assert call("mo-state", path=["map", 0, 1])["state"] == "three"
    long_map = call("mo-state", path=["long_map"])
    assert not long_map["complete"], long_map
    assert long_map["state"] == [[{"truncated_bytes": 7000}, "three"], ["y", "four"]], long_map
    assert call("mo-state", path=["long_map", 1, 1])["state"] == "four"
    assert call("mo_state") == "777"
    for name in NAMES:
        if name != "mo-export":
            call(name)
    retained = call("mo-failures")
    assert retained["records"] == [] and retained["truncated"] == 0, retained
    assert retained["next"] is None and retained["unavailable"] == [], retained
    for token in ["", "WrongOperatorCanary"]:
        status, _ = rpc("tools/call", {"name": "mo-state"}, token)
        assert status == 401
    for name in ["mo-export"]:
        status, body = rpc("tools/call", {"name": name})
        assert status == 200 and body["error"]["code"] == -32602
    for name, arguments in [("mo-state", {"path": [-0.0]}), ("mo-log", {"after": 1.0}),
                            ("mo-timers", {"before": 1}), ("mo-memory", {"offset": 0})]:
        status, body = rpc("tools/call", {"name": name, "arguments": arguments})
        assert status == 200 and body["error"]["code"] == -32602, (name, body)
    memory = call("mo-memory")
    sizes = {key: int(memory[key]) for key in
             ["heap_used", "heap_peak", "heap_limit", "stack_capacity", "stack_limit",
              "encoded_state_bytes", "diagnostic_scratch_peak"]}
    assert 0 < sizes["heap_used"] <= sizes["heap_peak"] <= sizes["heap_limit"], memory
    assert 0 < sizes["stack_capacity"] <= sizes["stack_limit"], memory
    assert sizes["encoded_state_bytes"] > 100000 + 7000, memory
    assert sizes["diagnostic_scratch_peak"] > 0, memory
    assert memory["pinned_export_bytes"] == 0 and memory["export_available"] is True
    timers = call("mo-timers")
    assert timers["complete"] and [t["name"] for t in timers["items"]] == ["tick"], timers
    assert timers["items"][0]["interval_us"] == "3600000000", timers
    assert call("mo-timers", offset=1)["items"] == []
    mailbox = call("mo-mailbox")
    assert mailbox["complete"] and mailbox["items"] == [], mailbox
    supervisor = call("mo-supervisor")
    assert supervisor["complete"] and supervisor["items"] == [], supervisor
    after = call("mo-overview")
    for key in ["recovery_sequence", "encoded_sequence", "snapshot_sequence", "failures", "restarts"]:
        assert after[key] == before[key], (key, before, after)
    assert int(after["uptime_us"]) > int(before["uptime_us"]), (before, after)
    log = call("mo-log")
    assert log["complete"] and len(log["items"]) == 1, log
    entry = log["items"][0]
    assert entry["timer"] and entry["argument_bytes"] == "0", log
    sequence = int(entry["sequence"])
    assert call("mo-log", after=sequence - 1)["items"] == [entry]
    later = call("mo-log", after=sequence)
    assert later["complete"] and later["items"] == [], later


def wide_state(_rpc, call):
    assert call("mo-state", path=["answer"])["state"] == "42"
    assert call("mo-state", path=["ints", 19999])["state"] == "20000"
    assert call("mo-state", path=["texts", 9999])["state"] == "10000"
    empties = call("mo-state", path=["empties"])
    shown = len(empties["state"]["items"])
    assert not empties["complete"] and empties["state"]["truncated"] == 6000 - shown, empties
    whole = call("mo-state")
    assert not whole["complete"] and whole["state"]["type"] == "Main.Wide", whole


def huge_state(_rpc, call):
    whole = call("mo-state")
    assert not whole["complete"], whole
    assert whole["state"]["fields"]["large"] == {"truncated_bytes": 16000000}, whole
    assert call("mo-state", path=["answer"])["state"] == "42"


def run(binary):
    binary = pathlib.Path(binary).resolve()
    serving(binary, SOURCE, private_state)
    serving(binary, WIDE, wide_state)
    serving(binary, SOURCE.replace("repeat(100000)", "repeat(16000000)"), huge_state)
    failure_records(binary)
    print("operator: private state, scalar types, paths, truncation, paging, routing, "
          "nonmutation and retained failures passed")


if __name__ == "__main__":
    run(sys.argv[1])
