module McpTransportCheck
use Json
use Mcp
use McpRpc
use McpTransport
use McpType

fn require(ok: Bool, _name: Text)
  if not ok
    stop "MCP transport contract"
  end
end

fn descriptor(count: Int) : McpType.Service
  var tools = []
  for i in 0..count
    tools = tools.append(
      McpType.Tool(
        name: "tool#{i}",
        description: "fixture",
        read_only: true,
        arguments: [],
        reply: 0,
        error: None
      )
    )
  end
  McpType.Service(types: [McpType.UnitType], tools: tools)
end

fn receive(text: Text, version: Option(Text)) : McpTransport.Action
  shape = descriptor(1)
  catalog = Mcp.prepare(shape, "0123456789abcdef", 32768) or stop "catalog"
  McpTransport.receive(text, version, shape, catalog, "fixture-version")
end

fn request(name: Text, params: Json.Json) : Text
  Json.Object({
    "jsonrpc": Json.String("2.0"),
    "id": Json.int(-7),
    "method": Json.String(name),
    "params": params
  }).to_text
end

fn error(id: Option(Json.Json), code: Int, message: Text) : McpTransport.Action
  status = if code == -32700 or code == -32600: 400 else: 200
  McpTransport.Reply(status: status, body: McpRpc.error(id, code, message).to_text)
end

fn envelopes()
  version = Some("2025-11-25")
  for text in ["{", "{\"x\":1,\"\\u0078\":2}"]
    require(receive(text, None) == error(None, -32700, "Parse error"), "parse error")
  end
  require(
    receive("[]", version) == error(None, -32600, "Invalid Request: batches are not supported"),
    "batch"
  )
  for text in ["null", "{}", "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}"]
    require(receive(text, version) == error(None, -32600, "Invalid Request"), "envelope")
  end
  require(
    receive("{\"jsonrpc\":\"2.0\",\"result\":{}}", version) ==
      McpTransport.Reply(status: 400, body: ""),
    "unsolicited"
  )
  unsupported = McpTransport.Reply(
    status: 400,
    body: "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"Unsupported protocol version: send MCP-Protocol-Version: 2025-11-25\",\"data\":{\"supported\":[\"2025-11-25\"]}}}"
  )
  require(receive(request("ping", Json.Object({})), None) == unsupported, "version")
  for method in ["initialize", "ping", "tools/list", "tools/call", "unknown"]
    for params in [Json.Array([]), Json.Object({"_meta": Json.Null})]
      notification = Json.Object({
        "jsonrpc": Json.String("2.0"),
        "method": Json.String(method),
        "params": params
      })
      require(
        receive(notification.to_text, version) == McpTransport.Reply(status: 202, body: ""),
        "notification"
      )
    end
  end
end

fn json(text: Text) : Json.Json
  case Json.parse(text)
    Ok(value): value
    Error(_): stop "transport fixture JSON"
  end
end

fn methods()
  version = Some("2025-11-25")
  for (name, params, message) in [
    ("ping", "[]", "Invalid params: params must be an object"),
    ("ping", "{\"_meta\":null}", "Invalid params: params must be an object"),
    (
      "initialize",
      "{}",
      "Invalid params: initialize needs protocolVersion, capabilities and clientInfo"
    ),
    (
      "initialize",
      "{\"protocolVersion\":\"old\",\"capabilities\":{},\"clientInfo\":{\"name\":\"client\"}}",
      "Invalid params: initialize needs protocolVersion, capabilities and clientInfo"
    ),
    ("tools/list", "{\"cursor\":\"1.0123456789abcdef\"}", "Invalid params: invalid cursor"),
    ("tools/call", "{}", "Invalid params: name must be a string"),
    ("tools/call", "{\"name\":4}", "Invalid params: name must be a string"),
    (
      "tools/call",
      "{\"name\":\"secret-input\",\"arguments\":[]}",
      "Invalid params: arguments must be an object"
    ),
    ("tools/call", "{\"name\":\"secret-input\"}", "Invalid params: unknown tool")
  ]
    require(
      receive(request(name, json(params)), version) == error(Some(Json.int(-7)), -32602, message),
      "method params"
    )
  end
  require(
    receive(request("secret-input", Json.Array([])), version) ==
      error(Some(Json.int(-7)), -32601, "Method not found"),
    "unknown method before params"
  )
  require(
    receive(request("ping", Json.Object({})), version) ==
      McpTransport.Reply(status: 200, body: "{\"jsonrpc\":\"2.0\",\"id\":-7,\"result\":{}}"),
    "ping"
  )
  initialize = request(
    "initialize",
    json(
      "{\"protocolVersion\":\"old\",\"capabilities\":{},\"clientInfo\":{\"name\":\"client\",\"version\":\"1\"}}"
    )
  )
  require(
    receive(initialize, None) ==
      McpTransport.Reply(
        status: 200,
        body: "{\"jsonrpc\":\"2.0\",\"id\":-7,\"result\":{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"mo\",\"version\":\"fixture-version\"}}}"
      ),
    "initialize ignores version"
  )
  for params in [
    Json.Object({"name": Json.String("tool0")}),
    json("{\"name\":\"tool0\",\"arguments\":{\"not_typed\":true}}")
  ]
    input = request("tools/call", params)
    rpc = case McpRpc.parse(input, version)
      McpRpc.Message(value): value
      McpRpc.Rejected(_) | McpRpc.WrongVersion | McpRpc.Unsolicited | McpRpc.Notification(params: _): stop "call fixture"
    end
    arguments = case params
      Json.Object(fields): fields.get("arguments") or Json.Object({})
      Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): stop "call params"
    end
    require(
      receive(input, version) == McpTransport.Call(request: rpc, tool: 0, arguments: arguments),
      "untyped call action"
    )
  end
end

fn pages()
  key = "0123456789abcdef"
  id = Json.int(-7)
  for count in [0, 31, 32, 33, 64]
    shape = descriptor(count)
    catalog = Mcp.prepare(shape, key, 32768) or stop "page catalog"
    var fields = {"tools": Json.Array(catalog.tools.take(32))}
    if count > 32
      fields = fields.set("nextCursor", Json.String("32.#{key}"))
    end
    expected = McpRpc.result(id, Json.Object(fields))
    require(Mcp.page(catalog, 0, id) == Some(expected), "page max 32")
    require(
      McpTransport.receive(
        request("tools/list", Json.Object({})),
        Some("2025-11-25"),
        shape,
        catalog,
        "v"
      ) ==
        McpTransport.Reply(status: 200, body: expected.to_text),
      "list response"
    )
    if count > 32
      last = McpRpc.result(id, Json.Object({"tools": Json.Array(catalog.tools.drop(32))}))
      require(Mcp.page(catalog, 32, id) == Some(last), "last page")
    end
  end
  empty = descriptor(0)
  worst = Json.String("\u{0}".repeat(128))
  expected = McpRpc.result(worst, Json.Object({"tools": Json.Array([])}))
  bound = expected.to_text.byte_count
  catalog = Mcp.prepare(empty, key, bound) or stop "empty catalog at bound"
  require(Mcp.prepare(empty, key, bound - 1) == None, "empty catalog exceeds bound")
  require(Mcp.page(catalog, 0, worst) == Some(expected), "empty tools")
  require(Mcp.page(catalog, 1, id) == None, "empty page invalid start")
  broken = Mcp.Catalog(tools: [], sizes: [], image_key: key, response_bytes: 0)
  require(
    McpTransport.receive(
      request("tools/list", Json.Object({})),
      Some("2025-11-25"),
      empty,
      broken,
      "v"
    ) ==
      error(Some(id), -32603, "Internal error"),
    "internal error"
  )
end

fn fallbacks()
  for (code, message) in [
    ("invalid_arguments", "the arguments do not match the tool's input schema"),
    ("busy", "service mailbox is full; retry later"),
    ("stopped", "the service is stopped"),
    ("memory_limit", "the call ran out of memory; service state is unchanged"),
    ("work_limit", "the call ran out of instructions; service state is unchanged"),
    ("handler_failed", "the tool handler failed; service state is unchanged"),
    ("store_failed", "saving service state failed; whether this call took effect is unknown"),
    ("rate_limited", "too many calls; retry later"),
    ("response_limit", "the tool result is larger than the response limit"),
    ("internal_error", "internal error in the Mo runtime")
  ]
    for effect in ["none", "applied", "unknown"]
      content = Json.Object({
        "error": Json.Object({
          "kind": Json.String("runtime"),
          "code": Json.String(code),
          "message": Json.String(message),
          "effect": Json.String(effect)
        })
      })
      expected = Json.Object({
        "content": Json.Array([
          Json.Object({"type": Json.String("text"), "text": Json.String(content.to_text)})
        ]),
        "structuredContent": content,
        "isError": Json.Boolean(true)
      })
      require(McpTransport.runtime_error(code, effect) == expected, "runtime error copies")
      (before, after) = McpTransport.fallback(code, effect)
      for id in [
        json("-0"),
        Json.String("\u{0}".repeat(128)),
        Json.String("\u{0}mo-runtime-rpc-id\u{0}"),
        Json.String("\"\\\n é")
      ]
        text = "#{before}#{id.to_text}#{after}"
        require(text == McpTransport.reply(id, expected), "fallback exact bytes")
        require(json(text) == McpRpc.result(id, expected), "fallback parse")
      end
    end
  end
  require(
    McpTransport.reply(json("-0"), Json.String("é")) ==
      "{\"jsonrpc\":\"2.0\",\"id\":-0,\"result\":\"é\"}",
    "reply exact"
  )
end

fn tool_replies()
  for failed in [false, true]
    for id in [json("-0"), Json.String("é\"\\\n\u{0}")]
      for value in [Json.Null, Json.String("é😀\"\\\n\r\t\u{0}\u{1f}"), json("{\"a\":[1,true]}")]
        result = if failed: Mcp.application_error(value) else: Mcp.success(value)
        expected = McpTransport.reply(id, result)
        bound = expected.byte_count
        require(Mcp.tool_reply(id, value, failed, bound) == Some(expected), "tool reply at bound")
        require(Mcp.tool_reply(id, value, failed, bound - 1) == None, "tool reply exceeds bound")
      end
    end
    for bound in [0, -1]
      require(Mcp.tool_reply(Json.int(1), Json.Null, failed, bound) == None, "tool reply no room")
    end
    require(
      Mcp.tool_reply(Json.int(1), Json.String("\u{0}".repeat(32768)), failed, 32768) == None,
      "oversized NUL tool reply"
    )
  end
end

fn http()
  require(
    McpTransport.http_head(200, 770) ==
      "HTTP/1.1 200 OK\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Length: 770\r\nContent-Type: application/json\r\n\r\n",
    "precomputed HTTP head"
  )
  for (status, body) in [(200, "é"), (202, ""), (401, ""), (405, "")]
    require(
      "#{McpTransport.http_head(status, body.byte_count)}#{body}" == McpTransport.http(status, body),
      "shared HTTP head policy"
    )
  end
  require(
    McpTransport.http(200, "é") ==
      "HTTP/1.1 200 OK\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Length: 2\r\nContent-Type: application/json\r\n\r\né",
    "HTTP UTF-8 length"
  )
  require(
    McpTransport.http(202, "") ==
      "HTTP/1.1 202 Accepted\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Length: 0\r\n\r\n",
    "HTTP empty"
  )
  require(
    McpTransport.http(401, "") ==
      "HTTP/1.1 401 Unauthorized\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Length: 0\r\nWWW-Authenticate: Bearer\r\n\r\n",
    "D316 bearer challenge"
  )
  require(
    McpTransport.http(405, "") ==
      "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Length: 0\r\nAllow: POST\r\n\r\n",
    "Allow POST"
  )
  require(McpTransport.http100() == "HTTP/1.1 100 Continue\r\n\r\n", "interim")
  require(McpTransport.http(100, "") == McpTransport.http100(), "interim HTTP")
end

fn nulls(count: Int) : Text
  if count == 0: "" else: "#{"null,".repeat(count - 1)}null"
end

fn call?(action: McpTransport.Action) : Bool
  case action
    McpTransport.Call(request: _, tool: index, arguments: _): index == 0
    McpTransport.Reply(status: _, body: _): false
  end
end

fn limits()
  version = Some("2025-11-25")
  refused = McpTransport.Reply(status: 413, body: "")
  for (count, expected) in [(58, false), (59, true)]
    text = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":-7,\"params\":{\"x\":[#{nulls(count)}]}}"
    if expected
      require(receive(text, version) == refused, "envelope 65")
    else
      require(
        receive(text, version) ==
          McpTransport.Reply(status: 200, body: "{\"jsonrpc\":\"2.0\",\"id\":-7,\"result\":{}}"),
        "envelope 64 keys not nodes"
      )
    end
  end
  for count in [16382, 16383]
    text = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":1,\"params\":{\"name\":\"tool0\",\"arguments\":{\"x\":[#{nulls(
      count
    )}]},\"extra\":[#{nulls(57)}]}}"
    if count == 16382
      require(call?(receive(text, version)), "payload 16384 plus envelope 64")
    else
      require(receive(text, version) == refused, "payload 16385")
      require(receive(text, None) == refused, "nodes before version")
    end
  end
  for count in [57, 58]
    text = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":1,\"params\":{\"name\":\"tool0\",\"arguments\":{},\"extra\":[#{nulls(
      count
    )}]}}"
    if count == 57
      require(call?(receive(text, version)), "call envelope 64")
    else
      require(receive(text, version) == refused, "call envelope 65")
    end
  end
  for depth in [57, 58, 125, 126]
    nested = "#{"[".repeat(depth)}0#{"]".repeat(depth)}"
    text = "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":1,\"params\":{\"name\":\"tool0\",\"arguments\":{\"x\":#{nested}}}}"
    if depth <= 125
      require(call?(receive(text, version)), "JSON depth through 128 before typed conversion")
    else
      require(receive(text, version) == error(None, -32700, "Parse error"), "JSON depth 129")
    end
  end
  large_other = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"params\":{\"arguments\":[#{nulls(64)}]}}"
  require(receive(large_other, version) == refused, "other method has no payload allowance")
  require(
    receive(
      "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"params\":{\"arguments\":[#{nulls(16384)}]}}",
      version
    ) ==
      refused,
    "notification node limit"
  )
end

fn main() : Int
  envelopes()
  methods()
  pages()
  fallbacks()
  tool_replies()
  http()
  limits()
  37
end
