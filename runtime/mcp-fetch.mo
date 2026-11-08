module McpFetch
expose read, upgrade
use Json
use Http

fn text(value: Json.Json, key: Text) : Text
  case value
    Json.Object(fields):
      case fields.get(key)
        Some(Json.String(s)): s
        Some(Json.Number(n)): Json.Number(n).to_text
        Some(Json.Null): ""
        Some(_) | None: "!"
      end
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): "!"
  end
end

## The result object of a well-formed HTTP/MCP reply to request id 1.
fn envelope(wire: Text) : Option(Map(Text, Json.Json))
  parts = wire.split("\r\n\r\n")
  return None if parts.size < 2
  head = parts.first or ""
  body = parts.drop(1).join("\r\n\r\n")
  return None if head.byte_count + 4 > 1024 or body.byte_count > 32768
  lines = head.split("\r\n")
  status = lines.first or ""
  return None if status != "HTTP/1.1 200 OK" and status != "HTTP/1.0 200 OK"
  var length = None
  var content_type = false
  for line in lines.drop(1)
    return None if line.chars.any?(fn(c) (c.code < 32 and c != '\t') or c.code == 127 end)
    fields = line.split(":")
    return None if fields.size < 2
    name = (fields.first or "").ascii_lowercase
    value = fields.drop(1).join(":").trim
    return None if name == "" or not name.chars.all?(fn(c) Http.token?(c.code) end)
    if name == "content-length"
      return None if length != None
      length = value.to_int
      return None if length.map(fn(n) n.to_text end) != Some(value)
    else if name == "transfer-encoding"
      return None
    else if name == "content-type"
      return None if content_type or value != "application/json"
      content_type = true
    end
  end
  return None if length != Some(body.byte_count) or not content_type
  envelope = case Json.parse(body)
    Ok(value): value
    Error(_): return None
  end
  envelope_fields = case envelope
    Json.Object(entries): entries
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): return None
  end
  return None if envelope_fields.get("jsonrpc") != Some(Json.String("2.0")) or
    envelope_fields.get("id") != Some(Json.int(1))
  return None if envelope_fields.get("error") != None
  result = case envelope
    Json.Object(fields): fields.get("result") or Json.Null
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): return None
  end
  case result
    Json.Object(fields): Some(fields)
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): None
  end
end

## A closed HTTP/MCP result: empty refuses, one recognized export error or ten page fields.
## C validates byte arithmetic and integrity before publication.
fn read(wire: Text) : List(Text)
  fields = case envelope(wire)
    Some(value): value
    None: return []
  end
  structured = fields.get("structuredContent") or Json.Null
  content = case structured
    Json.Object(entries): entries
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): return []
  end
  page = content.get("value") or Json.Null
  if fields.get("isError") == Some(Json.Boolean(true))
    error = text(page, "error")
    return [error] if error == "export_replaced" or error == "export_released"
    return []
  end
  return [] if fields.get("isError") != Some(Json.Boolean(false))
  page_fields = case page
    Json.Object(entries): entries
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): return []
  end
  return [] if (
    page_fields.get("kind") != Some(Json.String("state")) and
      page_fields.get("kind") != Some(Json.String("failure"))
  ) or
    page_fields.get("version") != Some(Json.int(1))
  for key in ["export", "sequence", "encoded_sequence", "checksum", "base64"]
    case page_fields.get(key)
      Some(Json.String(_)): Unit
      Some(_) | None: return []
    end
  end
  for key in ["offset", "length", "total", "pages"]
    case page_fields.get(key)
      Some(Json.Number(_)): Unit
      Some(_) | None: return []
    end
  end
  case page_fields.get("digest")
    Some(Json.Null) | Some(Json.String(_)): Unit
    Some(_) | None: return []
  end
  offset = text(page, "offset").to_int or -1
  size = text(page, "length").to_int or -1
  total = text(page, "total").to_int or -1
  next = page_fields.get("next") or Json.Boolean(false)
  if offset + size == total
    return [] if next != Json.Null
  else
    return [] if page_fields.get("digest") != Some(Json.Null)
    var expected = {
      "kind": page_fields.get("kind") or Json.Null,
      "export": Json.String(text(page, "export")),
      "offset": Json.int(offset + size)
    }
    if page_fields.get("kind") == Some(Json.String("failure"))
      failure = text(page, "failure").to_int or 0
      return [] if failure <= 0
      expected = expected.set("failure", Json.int(failure))
    end
    return [] if next != Json.Object(expected)
  end
  [
    text(page, "export"),
    text(page, "sequence"),
    text(page, "encoded_sequence"),
    text(page, "offset"),
    text(page, "length"),
    text(page, "total"),
    text(page, "pages"),
    text(page, "checksum"),
    text(page, "digest"),
    text(page, "base64")
  ]
end

## A closed upgrade-tool result: the error flag, the text rendering, the structured report and
## the slot an open names ("" otherwise). Empty refuses the reply as malformed.
fn upgrade(wire: Text) : List(Text)
  fields = case envelope(wire)
    Some(value): value
    None: return []
  end
  failed = case fields.get("isError")
    Some(Json.Boolean(value)): value
    Some(_) | None: return []
  end
  if failed
    case fields.get("structuredContent")
      Some(Json.Object(content)):
        case content.get("error")
          Some(Json.Object(error)):
            if error.get("kind") == Some(Json.String("runtime")) and
              error.get("code") == Some(Json.String("rate_limited")) and
              error.get("effect") == Some(Json.String("none"))
              return ["rate_limited"]
            end
          Some(_) | None: Unit
        end
      Some(_) | None: Unit
    end
  end
  structured = fields.get("structuredContent") or Json.Null
  case structured
    Json.Object(_): Unit
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): return []
  end
  rendered = case fields.get("content")
    Some(Json.Array(items)):
      case items.get(0)
        Some(item): text(item, "text")
        None: return []
      end
    Some(_) | None: return []
  end
  return [] if rendered == "!"
  slot = text(structured, "slot")
  [if failed: "true" else: "false", rendered, structured.to_text, if slot == "!": "" else: slot]
end
