module McpRpcCheck
use Json
use Mcp
use McpRpc
use McpType

fn require(ok: Bool)
  if not ok
    stop "MCP RPC contract"
  end
end

fn json(text: Text) : Json.Json
  case Json.parse(text)
    Ok(value): value
    Error(_): stop "RPC fixture JSON"
  end
end

fn request(text: Text) : McpRpc.Request
  case McpRpc.parse(text, Some("2025-11-25"))
    McpRpc.Message(value): value
    McpRpc.Rejected(_) | McpRpc.WrongVersion | McpRpc.Unsolicited | McpRpc.Notification(params: _): stop "RPC fixture request"
  end
end

fn envelopes()
  for (text, expected) in [
    ("[]", McpRpc.Rejected(-32600)),
    ("null", McpRpc.Rejected(-32600)),
    ("42", McpRpc.Rejected(-32600)),
    ("true", McpRpc.Rejected(-32600)),
    ("\"text\"", McpRpc.Rejected(-32600)),
    ("{}", McpRpc.Rejected(-32600)),
    ("{\"jsonrpc\":\"1.0\",\"method\":\"ping\",\"id\":1}", McpRpc.Rejected(-32600)),
    ("{\"jsonrpc\":\"2.0\",\"method\":7,\"id\":1}", McpRpc.Rejected(-32600)),
    ("{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"result\":null}", McpRpc.Rejected(-32600)),
    ("{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"error\":null}", McpRpc.Rejected(-32600)),
    ("{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"params\":null}", McpRpc.Rejected(-32600)),
    ("{\"jsonrpc\":\"2.0\",\"result\":{}}", McpRpc.Unsolicited),
    ("{\"jsonrpc\":\"2.0\",\"error\":{},\"id\":null}", McpRpc.Unsolicited),
    ("{\"jsonrpc\":\"2.0\",\"result\":{},\"id\":[],\"params\":null}", McpRpc.Unsolicited),
    ("{\"method\":\"initialize\",\"jsonrpc\":\"1.0\"}", McpRpc.Rejected(-32600))
  ]
    require(McpRpc.parse(text, Some("2025-11-25")) == expected)
  end
  for text in ["{}", "{\"jsonrpc\":\"1.0\",\"method\":7}", "{\"result\":{}}"]
    require(McpRpc.parse(text, None) == McpRpc.WrongVersion)
    require(McpRpc.parse(text, Some("2024-11-05")) == McpRpc.WrongVersion)
  end
  require(
    McpRpc.parse("{\"method\":\"initialize\",\"jsonrpc\":\"1.0\"}", None) == McpRpc.Rejected(-32600)
  )
  for text in [
    "{",
    "[1,]",
    "{\"a\":1,\"a\":2}",
    "{\"a\":1,\"\\u0061\":2}",
    "{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"id\":1,\"params\":{\"name\":\"echo\",\"arguments\":{\"a\":1,\"\\u0061\":2}}}"
  ]
    require(McpRpc.parse(text, None) == McpRpc.Rejected(-32700))
    require(McpRpc.parse(text, Some("2025-11-25")) == McpRpc.Rejected(-32700))
  end
  for name in [
    "initialize",
    "ping",
    "tools/list",
    "tools/call",
    "notifications/initialized",
    "unknown"
  ]
    text = Json.Object({
      "jsonrpc": Json.String("2.0"),
      "method": Json.String(name),
      "params": Json.Object({"task": Json.Null, "_meta": Json.Null})
    }).to_text
    require(
      McpRpc.parse(text, Some("2025-11-25")) ==
        McpRpc.Notification(params: json("{\"task\":null,\"_meta\":null}"))
    )
  end
end

fn ids()
  for spelling in [
    "0",
    "-0",
    "9223372036854775808",
    "9".repeat(128),
    "-#{"9".repeat(127)}",
    "\"\"",
    "\"01\"",
    Json.String("é".repeat(64)).to_text,
    Json.String("\u{0}".repeat(128)).to_text
  ]
    value = request("{\"jsonrpc\":\"2.0\",\"id\":#{spelling},\"method\":\"ping\"}")
    require(value.id == json(spelling))
    require(value.id_text == spelling)
    expected = json(
      "{\"jsonrpc\":\"2.0\",\"id\":#{spelling},\"error\":{\"code\":-32602,\"message\":\"fixture\"}}"
    )
    require(McpRpc.error(Some(value.id), -32602, "fixture") == expected)
    require(
      McpRpc.result(value.id, Json.Object({})) ==
        json("{\"jsonrpc\":\"2.0\",\"id\":#{spelling},\"result\":{}}")
    )
  end
  for spelling in [
    "null",
    "true",
    "{}",
    "[]",
    "1.0",
    "1e0",
    "9".repeat(129),
    "-#{"9".repeat(128)}",
    Json.String("#{"é".repeat(64)}a").to_text
  ]
    require(
      McpRpc.parse(
        "{\"jsonrpc\":\"2.0\",\"id\":#{spelling},\"method\":\"ping\"}",
        Some("2025-11-25")
      ) ==
        McpRpc.Rejected(-32600)
    )
  end
  require(
    McpRpc.error(None, -32700, "fixture") ==
      json("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,\"message\":\"fixture\"}}")
  )
end

fn methods()
  for (name, params, expected) in [
    (
      "initialize",
      "{\"protocolVersion\":\"old\",\"capabilities\":{},\"clientInfo\":{\"name\":\"client\",\"version\":\"1\",\"extra\":null},\"extra\":true}",
      McpRpc.Initialize
    ),
    (
      "initialize",
      "{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"client\",\"version\":\"1\"}}",
      McpRpc.Initialize
    ),
    (
      "initialize",
      "{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{},\"clientInfo\":{\"name\":\"client\"}}",
      McpRpc.InvalidParams
    ),
    (
      "initialize",
      "{\"protocolVersion\":2,\"capabilities\":{},\"clientInfo\":{\"name\":\"client\",\"version\":\"1\"}}",
      McpRpc.InvalidParams
    ),
    ("initialize", "{}", McpRpc.InvalidParams),
    ("ping", "{\"result\":null,\"error\":false,\"_meta\":{}}", McpRpc.Ping),
    ("tools/list", "{}", McpRpc.ListTools(0)),
    ("tools/list", "{\"cursor\":\"0.0123456789abcdef\"}", McpRpc.ListTools(0)),
    ("tools/list", "{\"cursor\":\"1.0123456789abcdef\"}", McpRpc.ListTools(1)),
    ("tools/call", "{\"name\":\"echo\"}", McpRpc.Call(tool: 1, arguments: Json.Object({}))),
    (
      "tools/call",
      "{\"name\":\"echo\",\"arguments\":{\"n\":1},\"task\":true,\"extra\":[]}",
      McpRpc.Call(tool: 1, arguments: json("{\"n\":1}"))
    ),
    ("tools/call", "{\"name\":\"missing\"}", McpRpc.InvalidParams),
    ("tools/call", "{\"name\":7}", McpRpc.InvalidParams),
    ("tools/call", "{\"name\":\"echo\",\"arguments\":[]}", McpRpc.InvalidParams),
    ("tools/call", "{\"name\":\"echo\",\"arguments\":null}", McpRpc.InvalidParams),
    ("unknown", "{\"_meta\":null}", McpRpc.UnknownMethod)
  ]
    input = request(
      "{\"jsonrpc\":\"2.0\",\"id\":-0,\"method\":\"#{name}\",\"params\":#{params},\"extra\":null}"
    )
    require(McpRpc.method(input, ["first", "echo"], "0123456789abcdef") == expected)
  end
  for name in ["initialize", "ping", "tools/list", "tools/call"]
    for params in ["[]", "{\"_meta\":null}", "{\"_meta\":[]}"]
      input = request("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"#{name}\",\"params\":#{params}}")
      require(McpRpc.method(input, ["echo"], "0123456789abcdef") == McpRpc.InvalidParams)
    end
  end
  for (name, expected) in [("ping", McpRpc.Ping), ("tools/list", McpRpc.ListTools(0))]
    input = request("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"#{name}\"}")
    require(McpRpc.method(input, ["echo"], "0123456789abcdef") == expected)
  end
  for cursor in [
    "2.0123456789abcdef",
    "00.0123456789abcdef",
    "-0.0123456789abcdef",
    "+1.0123456789abcdef",
    "1.0123456789abcdeF",
    "1.ffffffffffffffff",
    "1",
    ".0123456789abcdef",
    "1.0123456789abcdef.",
    "999999999999999999999999.0123456789abcdef"
  ]
    input = request(
      Json.Object({
        "jsonrpc": Json.String("2.0"),
        "id": Json.int(1),
        "method": Json.String("tools/list"),
        "params": Json.Object({"cursor": Json.String(cursor)})
      }).to_text
    )
    require(McpRpc.method(input, ["first", "echo"], "0123456789abcdef") == McpRpc.InvalidParams)
  end
  input = request(
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\",\"params\":{\"cursor\":0}}"
  )
  require(McpRpc.method(input, ["echo"], "0123456789abcdef") == McpRpc.InvalidParams)
  require(
    McpRpc.initialized("test-version") ==
      json(
        "{\"protocolVersion\":\"2025-11-25\",\"capabilities\":{\"tools\":{\"listChanged\":false}},\"serverInfo\":{\"name\":\"mo\",\"version\":\"test-version\"}}"
      )
  )
end

fn pages()
  var tools = []
  for name in ["first", "other", "third"]
    tools = tools.append(
      McpType.Tool(
        name: name,
        description: "escaped \"\n é",
        read_only: true,
        arguments: [],
        reply: 0,
        error: None
      )
    )
  end
  descriptor = McpType.Service(types: [McpType.UnitType], tools: tools)
  key = "0123456789abcdef"
  wide = Mcp.prepare(descriptor, key, 262144) or stop "wide catalog"
  all = Mcp.page(wide, 0, Json.int(-7)) or stop "whole page"
  require(
    all ==
      Json.Object({
        "jsonrpc": Json.String("2.0"),
        "id": Json.int(-7),
        "result": Json.Object({"tools": Json.Array(wide.tools)})
      })
  )
  id = Json.String("\u{0}".repeat(128))
  first = wide.tools.get(0) or stop "first tool"
  expected = Json.Object({
    "jsonrpc": Json.String("2.0"),
    "id": id,
    "result": Json.Object({"tools": Json.Array([first]), "nextCursor": Json.String("1.#{key}")})
  })
  bound = expected.to_text.byte_count
  tight = Mcp.prepare(descriptor, key, bound) or stop "tight catalog"
  require(Mcp.prepare(descriptor, key, bound - 1) == None)
  require(Mcp.page(tight, 0, id) == Some(expected))
  for i in 0..3
    output = Mcp.page(tight, i, id) or stop "one tool page"
    tool = wide.tools.get(i) or stop "tool index"
    var fields = {"tools": Json.Array([tool])}
    if i < 2
      fields = fields.set("nextCursor", Json.String("#{i + 1}.#{key}"))
    end
    require(
      output ==
        Json.Object({"jsonrpc": Json.String("2.0"), "id": id, "result": Json.Object(fields)})
    )
    require(output.to_text.byte_count <= bound)
  end
  require(Mcp.page(tight, -1, id) == None)
  require(Mcp.page(tight, 3, id) == None)
  for bad in ["0123456789abcdeF", "0123456789abcdeg", "0123456789abcde", "0123456789abcdef0"]
    require(Mcp.prepare(descriptor, bad, 262144) == None)
  end
end

fn main() : Int
  envelopes()
  ids()
  methods()
  pages()
  37
end
