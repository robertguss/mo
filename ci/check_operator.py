#!/usr/bin/env python3
"""Authenticated live views against a real persistent service, without author inspection tools."""
import json
import os
import pathlib
import signal
import sys
import tempfile
import time

from check_cli import SERVE_OPERATOR_NAMES as NAMES, Served

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


def serving(binary, source, check):
    with tempfile.TemporaryDirectory(prefix="mo-operator-") as directory:
        folder = pathlib.Path(directory)
        path = folder / "main.mo"
        path.write_text(source, encoding="utf-8")
        data = folder / "data"
        data.mkdir()
        served = Served(binary, path, data, {**os.environ, "OPERATOR_TOKEN": TOKEN}, folder)
        try:
            assert served.port, served.errors()

            def rpc(method, params, token=TOKEN):
                time.sleep(0.055)
                body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                                   "params": params})
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

            check(rpc, call)
            assert served.finish(0, signal.SIGTERM) == STOPPED
        finally:
            served.close()


def private_state(rpc, call):
    status, body = rpc("tools/list", {})
    tools = body["result"]["tools"]
    assert status == 200 and [tool["name"] for tool in tools] == ["mo_state", *NAMES]
    assert "annotations" not in tools[0]
    assert all(t["annotations"] == {"readOnlyHint": True, "idempotentHint": True}
               for t in tools[1:])
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
        call(name)
    for token in ["", "WrongOperatorCanary"]:
        status, _ = rpc("tools/call", {"name": "mo-state"}, token)
        assert status == 401
    for name in ["mo-export", "mo-failures"]:
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
    assert memory["pinned_export_bytes"] is None and memory["export_available"] is False
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
    print("operator: private state, scalar types, paths, truncation, paging, routing and "
          "nonmutation passed")


if __name__ == "__main__":
    run(sys.argv[1])
