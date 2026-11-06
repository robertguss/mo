module McpFetch
expose read
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

## A closed HTTP/MCP result: empty refuses, one recognized export error or ten page fields.
## C validates byte arithmetic and integrity before publication.
fn read(wire: Text) : List(Text)
  parts = wire.split("\r\n\r\n")
  return [] if parts.size < 2
  head = parts.first or ""
  body = parts.drop(1).join("\r\n\r\n")
  return [] if head.byte_count + 4 > 1024 or body.byte_count > 32768
  lines = head.split("\r\n")
  status = lines.first or ""
  return [] if status != "HTTP/1.1 200 OK" and status != "HTTP/1.0 200 OK"
  var length = None
  var content_type = false
  for line in lines.drop(1)
    return [] if line.chars.any?(fn(c) (c.code < 32 and c != '\t') or c.code == 127 end)
    fields = line.split(":")
    return [] if fields.size < 2
    name = (fields.first or "").ascii_lowercase
    value = fields.drop(1).join(":").trim
    return [] if name == "" or not name.chars.all?(fn(c) Http.token?(c.code) end)
    if name == "content-length"
      return [] if length != None
      length = value.to_int
      return [] if length.map(fn(n) n.to_text end) != Some(value)
    else if name == "transfer-encoding"
      return []
    else if name == "content-type"
      return [] if content_type or value != "application/json"
      content_type = true
    end
  end
  return [] if length != Some(body.byte_count) or not content_type
  envelope = case Json.parse(body)
    Ok(value): value
    Error(_): return []
  end
  envelope_fields = case envelope
    Json.Object(entries): entries
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): return []
  end
  return [] if envelope_fields.get("jsonrpc") != Some(Json.String("2.0")) or
    envelope_fields.get("id") != Some(Json.int(1))
  return [] if envelope_fields.get("error") != None
  result = case envelope
    Json.Object(fields): fields.get("result") or Json.Null
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): return []
  end
  fields = case result
    Json.Object(fields): fields
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): return []
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
  return [] if page_fields.get("kind") != Some(Json.String("state")) or
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
    return [] if next !=
      Json.Object({
        "kind": Json.String("state"),
        "export": Json.String(text(page, "export")),
        "offset": Json.int(offset + size)
      })
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
