module McpTransport
expose Action(..), receive, runtime_error, reply, fallback, http, http_head, http100
use Json
use Mcp
use McpRpc
use McpType

enum Action
  Reply(status: Int, body: Text)
  Call(request: McpRpc.Request, tool: Int, arguments: Json.Json)
end

fn receive(
  text: Text,
  version: Option(Text),
  descriptor: McpType.Service,
  catalog: Mcp.Catalog,
  server_version: Text
) : Action
  body = case Json.parse(text)
    Ok(value): value
    Error(_): return protocol_error(None, -32700, "Parse error")
  end
  return Reply(status: 413, body: "") if bounded_nodes(body) == None
  request = case McpRpc.classify(body, version)
    McpRpc.Rejected(_):
      message = case body
        Json.Array(_): "Invalid Request: batches are not supported"
        Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Object(_): "Invalid Request"
      end
      return protocol_error(None, -32600, message)
    McpRpc.WrongVersion: return Reply(
      status: 400,
      body: Json.Object({
        "jsonrpc": Json.String("2.0"),
        "error": Json.Object({
          "code": Json.int(-32600),
          "message": Json.String(
            "Unsupported protocol version: send MCP-Protocol-Version: 2025-11-25"
          ),
          "data": Json.Object({"supported": Json.Array([Json.String("2025-11-25")])})
        })
      }).to_text
    )
    McpRpc.Unsolicited: return Reply(status: 400, body: "")
    McpRpc.Notification(params: _): return Reply(status: 202, body: "")
    McpRpc.Message(value): value
  end
  tools = descriptor.tools.map(fn(tool) tool.name end)
  method = case McpRpc.validate(request, tools, catalog.image_key)
    Ok(value): value
    Error(message): return protocol_error(Some(request.id), -32602, message)
  end
  case method
    McpRpc.InvalidParams | McpRpc.UnknownMethod:
      protocol_error(Some(request.id), -32601, "Method not found")
    McpRpc.Initialize: Reply(
      status: 200,
      body: reply(request.id, McpRpc.initialized(server_version))
    )
    McpRpc.Ping: Reply(status: 200, body: reply(request.id, Json.Object({})))
    McpRpc.ListTools(start):
      case Mcp.page(catalog, start, request.id)
        Some(value): Reply(status: 200, body: value.to_text)
        None: protocol_error(Some(request.id), -32603, "Internal error")
      end
    McpRpc.Call(tool: index, arguments: arguments): Call(
      request: request,
      tool: index,
      arguments: arguments
    )
  end
end

fn protocol_error(id: Option(Json.Json), code: Int, message: Text) : Action
  status = if code == -32700 or code == -32600: 400 else: 200
  Reply(status: status, body: McpRpc.error(id, code, message).to_text)
end

fn bounded_nodes(body: Json.Json) : Option(Int)
  fields = case body
    Json.Object(value): value
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): return nodes(
      body,
      64
    )
  end
  if fields.get("method") != Some(Json.String("tools/call"))
    return nodes(body, 64)
  end
  var envelope = 1
  for (key, value) in fields.entries
    if key == "params"
      case value
        Json.Object(params):
          envelope += 1
          return None if envelope > 64
          for (name, item) in params.entries
            if name == "arguments"
              return None if nodes(item, 16384) == None
            else
              count = try nodes(item, 64 - envelope)
              envelope += count
            end
          end
        Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_):
          count = try nodes(value, 64 - envelope)
          envelope += count
      end
    else
      count = try nodes(value, 64 - envelope)
      envelope += count
    end
  end
  Some(envelope)
end

fn nodes(value: Json.Json, limit: Int) : Option(Int)
  return None if limit < 1
  var count = 1
  case value
    Json.Array(items):
      for item in items
        count += try nodes(item, limit - count)
      end
    Json.Object(fields):
      for (_key, item) in fields.entries
        count += try nodes(item, limit - count)
      end
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_): Unit
  end
  Some(count)
end

fn runtime_error(code: Text, effect: Text) : Json.Json
  message = case code
    "invalid_arguments": "the arguments do not match the tool's input schema"
    "busy": "service mailbox is full; retry later"
    "stopped": "the service is stopped"
    "memory_limit": "the call ran out of memory; service state is unchanged"
    "work_limit": "the call ran out of instructions; service state is unchanged"
    "handler_failed": "the tool handler failed; service state is unchanged"
    "store_failed": "saving service state failed; whether this call took effect is unknown"
    "rate_limited": "too many calls; retry later"
    "response_limit": "the tool result is larger than the response limit"
    _: "internal error in the Mo runtime"
  end
  Mcp.tool_result(
    Json.Object({
      "error": Json.Object({
        "kind": Json.String("runtime"),
        "code": Json.String(code),
        "message": Json.String(message),
        "effect": Json.String(effect)
      })
    }),
    true
  )
end

fn reply(id: Json.Json, value: Json.Json) : Text
  McpRpc.result(id, value).to_text
end

fn fallback(code: Text, effect: Text) : (Text, Text)
  marker = Json.String("\u{0}mo-runtime-rpc-id\u{0}")
  before = "{\"jsonrpc\":\"2.0\",\"id\":"
  complete = reply(marker, runtime_error(code, effect))
  after = complete.drop_prefix("#{before}#{marker.to_text}") or stop "MCP fallback ID slot"
  (before, after)
end

fn http(status: Int, body: Text) : Text
  "#{http_head(status, body.byte_count)}#{body}"
end

fn http_head(status: Int, body_bytes: Int) : Text
  return http100() if status == 100
  reason = case status
    200: "OK"
    202: "Accepted"
    400: "Bad Request"
    401: "Unauthorized"
    403: "Forbidden"
    404: "Not Found"
    405: "Method Not Allowed"
    406: "Not Acceptable"
    408: "Request Timeout"
    411: "Length Required"
    413: "Content Too Large"
    414: "URI Too Long"
    415: "Unsupported Media Type"
    417: "Expectation Failed"
    421: "Misdirected Request"
    431: "Request Header Fields Too Large"
    500: "Internal Server Error"
    501: "Not Implemented"
    503: "Service Unavailable"
    505: "HTTP Version Not Supported"
    _: ""
  end
  var headers = "HTTP/1.1 #{status} #{reason}\r\nConnection: close\r\nCache-Control: no-store\r\nContent-Length: #{body_bytes}\r\n"
  if body_bytes > 0
    headers = "#{headers}Content-Type: application/json\r\n"
  end
  if status == 401
    headers = "#{headers}WWW-Authenticate: Bearer\r\n"
  else if status == 405
    headers = "#{headers}Allow: POST\r\n"
  end
  "#{headers}\r\n"
end

fn http100() : Text
  "HTTP/1.1 100 Continue\r\n\r\n"
end
