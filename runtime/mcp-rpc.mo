module McpRpc
expose Request(..),
Envelope(..),
Method(..),
parse,
classify,
method,
validate,
result,
error,
initialized
use Json

struct Request
  id: Json.Json
  id_text: Text
  method: Text
  params: Json.Json
end

enum Envelope
  Rejected(Int)
  WrongVersion
  Unsolicited
  Notification(params: Json.Json)
  Message(Request)
end

enum Method
  InvalidParams
  UnknownMethod
  Initialize
  Ping
  ListTools(Int)
  Call(tool: Int, arguments: Json.Json)
end

fn parse(text: Text, version: Option(Text)) : Envelope
  body = case Json.parse(text)
    Ok(value): value
    Error(_): return Rejected(-32700)
  end
  classify(body, version)
end

fn classify(body: Json.Json, version: Option(Text)) : Envelope
  fields = case body
    Json.Object(value): value
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): return Rejected(
      -32600
    )
  end
  if fields.get("method") != Some(Json.String("initialize"))
    return WrongVersion if version != Some("2025-11-25")
  end
  return Rejected(-32600) if fields.get("jsonrpc") != Some(Json.String("2.0"))
  name = case fields.get("method")
    Some(Json.String(value)): value
    Some(_): return Rejected(-32600)
    None:
      if fields.get("result") != None or fields.get("error") != None
        return Unsolicited
      end
      return Rejected(-32600)
  end
  if fields.get("result") != None or fields.get("error") != None
    return Rejected(-32600)
  end
  params = fields.get("params") or Json.Object({})
  case params
    Json.Object(_): Unit
    Json.Array(_): Unit
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_): return Rejected(-32600)
  end
  id = case fields.get("id")
    Some(value): value
    None: return Notification(params: params)
  end
  return Rejected(-32600) if not valid_id(id)
  Message(Request(id: id, id_text: id.to_text, method: name, params: params))
end

fn valid_id(id: Json.Json) : Bool
  case id
    Json.String(text): text.byte_count <= 128
    Json.Number(_): integer_id(id.to_text)
    Json.Null | Json.Boolean(_) | Json.Array(_) | Json.Object(_): false
  end
end

fn integer_id(text: Text) : Bool
  return false if text.byte_count > 128
  chars = text.chars
  start = if chars.get(0) == Some('-'): 1 else: 0
  first = chars.get(start) or ' '
  return chars.size == start + 1 if first == '0'
  return false if first < '1' or first > '9'
  for c in chars.drop(start + 1)
    return false if c < '0' or c > '9'
  end
  true
end

fn method(request: Request, tools: List(Text), image_key: Text) : Method
  case validate(request, tools, image_key)
    Ok(value): value
    Error(_): InvalidParams
  end
end

fn validate(request: Request, tools: List(Text), image_key: Text) : Result(Method, Text)
  name = request.method
  if name != "initialize" and name != "ping" and name != "tools/list" and name != "tools/call"
    return Ok(UnknownMethod)
  end
  params = case request.params
    Json.Object(value): value
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): return Error(
      "Invalid params: params must be an object"
    )
  end
  case params.get("_meta")
    None: Unit
    Some(Json.Object(_)): Unit
    Some(_): return Error("Invalid params: params must be an object")
  end
  if name == "initialize"
    case (params.get("protocolVersion"), params.get("capabilities"), params.get("clientInfo"))
      (Some(Json.String(_)), Some(Json.Object(_)), Some(Json.Object(info))):
        case (info.get("name"), info.get("version"))
          (Some(Json.String(_)), Some(Json.String(_))): Ok(Initialize)
          _: Error("Invalid params: initialize needs protocolVersion, capabilities and clientInfo")
        end
      _: Error("Invalid params: initialize needs protocolVersion, capabilities and clientInfo")
    end
  else if name == "ping"
    Ok(Ping)
  else if name == "tools/list"
    case params.get("cursor")
      None: Ok(ListTools(0))
      Some(Json.String(text)):
        parts = text.split(".")
        return Error("Invalid params: invalid cursor") if parts.size != 2 or
          parts.get(1) != Some(image_key)
        spelling = parts.get(0) or stop "MCP cursor index"
        index = spelling.to_int or -1
        if index < 0 or index >= tools.size or index.to_text != spelling
          return Error("Invalid params: invalid cursor")
        end
        Ok(ListTools(index))
      Some(_): Error("Invalid params: invalid cursor")
    end
  else
    tool_name = case params.get("name")
      Some(Json.String(value)): value
      None | Some(_): return Error("Invalid params: name must be a string")
    end
    arguments = params.get("arguments") or Json.Object({})
    case arguments
      Json.Object(_): Unit
      Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): return Error(
        "Invalid params: arguments must be an object"
      )
    end
    for i in 0..tools.size
      return Ok(Call(tool: i, arguments: arguments)) if tools.get(i) == Some(tool_name)
    end
    Error("Invalid params: unknown tool")
  end
end

fn result(id: Json.Json, value: Json.Json) : Json.Json
  Json.Object({"jsonrpc": Json.String("2.0"), "id": id, "result": value})
end

fn error(id: Option(Json.Json), code: Int, message: Text) : Json.Json
  var fields = {
    "jsonrpc": Json.String("2.0"),
    "error": Json.Object({"code": Json.int(code), "message": Json.String(message)})
  }
  case id
    Some(value): fields = fields.set("id", value)
    None: Unit
  end
  Json.Object(fields)
end

fn initialized(version: Text) : Json.Json
  Json.Object({
    "protocolVersion": Json.String("2025-11-25"),
    "capabilities": Json.Object({"tools": Json.Object({"listChanged": Json.Boolean(false)})}),
    "serverInfo": Json.Object({"name": Json.String("mo"), "version": Json.String(version)})
  })
end
