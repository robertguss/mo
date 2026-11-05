#!/usr/bin/env python3
"""Authenticated live views against a real persistent service, without author inspection tools."""
import json
import os
import pathlib
import signal
import sys
import tempfile
import time

from check_cli import Served

TOKEN = "OperatorViewCanary"
NAMES = ["mo-overview", "mo-state", "mo-mailbox", "mo-timers", "mo-memory",
         "mo-supervisor", "mo-log"]
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
    set: [5, 2].to_set, items: [7, 9], large: "x".repeat(100000), long_map: {"x".repeat(2000): "three", "y": "four"})
end
fn mo_state(_state: Secret, _now: Time) : Int
  777
end
fn tick(state: Secret, _now: Time) : Secret
  state
end
'''


def run(binary):
    binary = pathlib.Path(binary).resolve()
    with tempfile.TemporaryDirectory(prefix="mo-operator-") as directory:
        folder = pathlib.Path(directory)
        source = folder / "main.mo"
        source.write_text(SOURCE)
        data = folder / "data"
        data.mkdir()
        served = Served(binary, source, data, {**os.environ, "OPERATOR_TOKEN": TOKEN}, folder)
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
                return body["result"]["structuredContent"]["value"]

            status, body = rpc("tools/list", {})
            tools = body["result"]["tools"]
            assert status == 200 and [tool["name"] for tool in tools] == ["mo_state", *NAMES]
            assert "annotations" not in tools[0]
            assert all(t["annotations"] == {"readOnlyHint": True, "idempotentHint": True}
                       for t in tools[1:])
            before = call("mo-overview")
            assert before["runtime_identity"] and len(before["runtime_identity"]) == 64
            whole = call("mo-state")
            assert not whole["complete"] and whole["state"]["fields"]["answer"] == "42", whole
            assert whole["state"]["fields"]["large"] == {"truncated": 100000}, whole
            expected = {"answer": "42", "text": "private é", "bytes": {"base64": "YWJj"},
                        "big": "-123456789012345678901234567890", "decimal": "-1.250000000000000001",
                        "float": 1.5, "time": "2026-11-05T01:02:03.123456Z",
                        "range": {"start": "3", "end": "7"}, "tuple": [True, "λ"],
                        "map": [["3", "three"]], "set": ["5", "2"], "items": ["7", "9"]}
            for key, value in expected.items():
                result = call("mo-state", path=[key])
                assert result["complete"] and result["state"] == value, (key, result)
            assert call("mo-state", path=["items", 1])["state"] == "9"
            assert call("mo-state", path=["map", 0])["state"] == ["3", "three"]
            assert call("mo-state", path=["map", 0, 1])["state"] == "three"
            truncated_map = call("mo-state", path=["long_map"])
            assert not truncated_map["complete"], truncated_map
            assert truncated_map["state"] == [[{"truncated": 2000}, {"truncated": 1}], {"truncated": 1}], truncated_map
            assert call("mo_state") == "777"
            for name in NAMES:
                call(name)
            for token in ["", "WrongOperatorCanary"]:
                status, _ = rpc("tools/call", {"name": "mo-state"}, token)
                assert status == 401
            for name in ["mo-export", "mo-failures"]:
                status, body = rpc("tools/call", {"name": name})
                assert status == 200 and body["error"]["code"] == -32602
            after = call("mo-overview")
            for key in ["recovery_sequence", "encoded_sequence", "snapshot_sequence", "failures", "restarts"]:
                assert after[key] == before[key], (key, before, after)
            log = call("mo-log")
            assert log["complete"] and len(log["items"]) == 1, log
            assert log["items"][0]["timer"] and log["items"][0]["argument_bytes"] == "0"
            served.finish(0, signal.SIGTERM)
        finally:
            served.close()
    with tempfile.TemporaryDirectory(prefix="mo-operator-large-") as directory:
        folder = pathlib.Path(directory)
        source = folder / "main.mo"
        source.write_text(SOURCE.replace("repeat(100000)", "repeat(16000000)"))
        data = folder / "data"
        data.mkdir()
        served = Served(binary, source, data, {**os.environ, "OPERATOR_TOKEN": TOKEN}, folder)
        try:
            assert served.port, served.errors()
            for path in [[], ["answer"]]:
                body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                   "params": {"name": "mo-state", "arguments": {"path": path}}})
                status, raw = served.post(body, TOKEN)
                result = json.loads(raw)["result"]
                assert status == 200 and not result["isError"], result
                value = result["structuredContent"]["value"]
                if path:
                    assert value["complete"] and value["state"] == "42", value
                else:
                    assert not value["complete"] and value["state"]["fields"]["large"] == {"truncated": 16000000}, value
                assert len(raw) <= 32768
            served.finish(0, signal.SIGTERM)
        finally:
            served.close()
    print("operator: private state, scalar types, paths, truncation, routing and nonmutation passed")


if __name__ == "__main__":
    run(sys.argv[1])
