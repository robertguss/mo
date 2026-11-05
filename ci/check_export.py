#!/usr/bin/env python3
"""Independent MOEX/page checks through actual authenticated HTTP; not mo fetch (U9)."""
import base64
import hashlib
import pathlib
import struct
import sys

from check_operator import serving

SOURCE = '''module Main
service
 state Text = "a".repeat(20000)
 tools change, count
 token from env "OPERATOR_TOKEN"
end
fn change(state: Text, _now: Time) : (Text, Int)
 ("#{state}b", state.byte_count + 1)
end
fn count(state: Text, _now: Time) : Int
 state.byte_count
end
'''


def fnv(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"{value:016x}"


def check(binary):
    build = hashlib.sha256(binary.read_bytes()).digest()

    def scenario(rpc, call):
        def refused(**arguments):
            status, body = rpc("tools/call", {"name": "mo-export", "arguments": arguments})
            assert status == 200 and body["result"]["isError"] is True, body
            value = body["result"]["structuredContent"]["value"]
            assert value["state_effect"] == "none", value
            return value

        status, catalog = rpc("tools/list", {})
        tool = next(t for t in catalog["result"]["tools"] if t["name"] == "mo-export")
        assert status == 200 and tool["inputSchema"]["required"] == ["kind"]
        assert tool["annotations"] == {"readOnlyHint": True, "idempotentHint": True,
                                       "openWorldHint": False}
        before = call("mo-overview")
        memory = call("mo-memory")
        first = call("mo-export", kind="state")
        assert first["pages"] == 3 and first["offset"] == 0 and first["length"] == 8192
        assert first["digest"] is None and first["sequence"] == first["encoded_sequence"] == "0"
        assert call("mo-memory")["pinned_export_bytes"] > first["total"]
        # Authentication refuses before replacing, reading, or refreshing the current generation.
        for token in ["", "WrongExportCanary"]:
            status, _ = rpc("tools/call", {"name": "mo-export", "arguments": {"kind": "state"}}, token)
            assert status == 401
        call("change")
        page, data, offset = first, bytearray(), 0
        while True:
            assert page["export"] == first["export"] and page["sequence"] == "0"
            assert page["encoded_sequence"] == "0" and page["total"] == first["total"]
            assert page["version"] == 1 and page["kind"] == "state" and page["pages"] == 3
            raw = base64.b64decode(page["base64"], validate=True)
            assert page["offset"] == offset and len(raw) == page["length"]
            assert fnv(raw) == page["checksum"]
            data.extend(raw)
            offset += len(raw)
            if page["next"] is None:
                break
            page = call("mo-export", **page["next"])
        assert offset == first["total"] and hashlib.sha256(data[:-32]).digest() == data[-32:]
        assert data[-32:].hex() == page["digest"]
        assert data[:6] == b"MOEX\1\1" and data[6:22].hex() == first["export"]
        assert struct.unpack_from("<Q", data, 22)[0] == int(before["program_identity"])
        assert data[30:62] == build
        label_size = struct.unpack_from("<I", data, 62)[0]
        assert 0 < label_size <= 256
        assert data[66:66 + label_size].decode() == before["runtime_build"]
        service, durable, encoded, created = struct.unpack_from("<IQQq", data, 66 + label_size)
        assert service == 0 and durable == encoded == 0 and created > 0
        schema_digest = data[94 + label_size:126 + label_size]
        body_size = struct.unpack_from("<I", data, 126 + label_size)[0]
        body = data[130 + label_size:-32]
        assert len(body) == body_size and body[:5] == b"MOSV\1"
        schema_size = struct.unpack_from("<I", body, 5)[0]
        assert hashlib.sha256(body[9:9 + schema_size]).digest() == schema_digest
        payload = body[9 + schema_size:]
        assert struct.unpack_from("<I", payload)[0] == 20000 and payload[4:] == b"a" * 20000
        stale = {"kind": "state", "export": first["export"], "offset": 8192}
        assert refused(**stale)["error"] == "export_released"
        assert call("mo-memory")["pinned_export_bytes"] == 0
        after = call("mo-overview")
        assert after["failures"] == before["failures"] and after["restarts"] == before["restarts"]
        assert after["recovery_sequence"] == "1" and after["mailbox_count"] == 0
        assert call("count") == "20001"
        # Same-sequence replacement has distinct IDs; stale reads never alias the new pin.
        second = call("mo-export", kind="state")
        third = call("mo-export", kind="state")
        assert second["sequence"] == third["sequence"] == "1"
        assert second["export"] != third["export"]
        assert refused(**second["next"])["error"] == "export_replaced"
        for arguments in [{"kind": "state", "offset": 8192},
                          {"kind": "failure"}, {"kind": "state", "export": "x" * 32},
                          {"kind": "state", "offset": -1}, {"kind": "state", "offset": 1},
                          {"kind": "state", "offset": 17825792},
                          {"kind": "state", "offset": 9223372036854775808},
                          {"kind": "state", "pages": 3}]:
            status, result = rpc("tools/call", {"name": "mo-export", "arguments": arguments})
            assert status == 200 and result["error"]["code"] == -32602, result
        # Invalid aligned offsets preserve the generation and do not refresh its bytes.
        assert refused(kind="state", export=third["export"], offset=32768)["error"] == "invalid_offset"
        assert call("mo-export", **third["next"])["export"] == third["export"]
        current = call("mo-memory")
        assert int(current["heap_used"]) == int(memory["heap_used"]) + 1

    serving(binary, SOURCE, scenario)
    print("export: independent MOEX framing, identities, schema/artifact/page digests, "
          "immutable commit trace, authentication, replacement and malformed requests passed")


if __name__ == "__main__":
    check(pathlib.Path(sys.argv[1]).resolve())
