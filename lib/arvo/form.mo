## Bounded `application/x-www-form-urlencoded` and `multipart/form-data` request body parsers.
module Arvo.Form
expose FormError(..),
UploadedFile(..),
MultipartForm(..),
parse_urlencoded,
parse_multipart,
field,
file
use Api

max_urlencoded_bytes = 1048576
max_urlencoded_fields = 1024
max_multipart_parts = 128
max_boundary_bytes = 70

## Why form or multipart request parsing failed.
enum FormError
  InvalidContentType
  BodyTooLarge
  InvalidUtf8
  MalformedMultipart(reason: Text)
  TooManyParts
end

## One file part uploaded in a `multipart/form-data` request.
struct UploadedFile
  field_name: Text
  filename: Text
  content_type: Text
  data: Bytes
end

## Decoded text fields and uploaded files from a `multipart/form-data` request.
struct MultipartForm
  fields: Map(Text, List(Text))
  files: List(UploadedFile)
end

## Returns the first value for `name` in `fields`, or `None` when absent.
fn field(fields: Map(Text, List(Text)), name: Text) : Option(Text)
  examples
    field({"email": ["ada@example.com", "second@example.com"]}, "email") == Some("ada@example.com")
    field({}, "email") == None
  end

  vals = try fields.get(name)
  vals.first
end

## Returns the first uploaded file matching `field_name`, or `None` when absent.
fn file(form: MultipartForm, field_name: Text) : Option(UploadedFile)
  form.files.find(fn(f) f.field_name == field_name end)
end

fn media_type(ct: Text) : Text
  (ct.split(";").first or "").trim.ascii_lowercase
end

fn append_field(fields: Map(Text, List(Text)), name: Text, val: Text) : Map(Text, List(Text))
  prev = fields.get(name) or []
  fields.set(name, prev.append(val))
end

## Parses an `application/x-www-form-urlencoded` request body into `Map(Text, List(Text))`.
fn parse_urlencoded(req: Api.Request) : Result(Map(Text, List(Text)), FormError)
  ct = req.header("Content-Type") or ""
  return Error(InvalidContentType) if media_type(ct) != "application/x-www-form-urlencoded"
  return Error(BodyTooLarge) if req.body.size > max_urlencoded_bytes
  raw = try req.body.decode_utf8.or_fail(InvalidUtf8)
  return Ok({}) if raw.empty?

  parts = raw.split("&")
  return Error(TooManyParts) if parts.size > max_urlencoded_fields

  var out = {}
  for part in parts
    continue if part.empty?
    pieces = part.split("=")
    k_raw = pieces.first or ""
    v_raw = if pieces.size > 1: pieces.drop(1).join("=") else: ""
    k = try Api.percent_decode(k_raw, true).or_fail(InvalidUtf8)
    v = try Api.percent_decode(v_raw, true).or_fail(InvalidUtf8)
    out = append_field(out, k, v)
  end
  Ok(out)
end

fn unquote_param(raw: Text) : Option(Text)
  trimmed = raw.trim
  if trimmed.starts_with?("\"")
    return None if trimmed.byte_count < 2 or not trimmed.ends_with?("\"")
    inner = trimmed.bytes.slice(1, trimmed.byte_count - 1).decode_utf8 or ""
    var chars = []
    var escaped = false
    for ch in inner.chars
      if escaped
        if ch == '"' or ch == '\\'
          chars = chars.append(ch)
        else
          chars = chars.append('\\').append(ch)
        end
        escaped = false
      else if ch == '\\'
        escaped = true
      else if ch == '"'
        return None
      else
        chars = chars.append(ch)
      end
    end
    return None if escaped
    Some(Text.from_chars(chars))
  else
    Some(trimmed)
  end
end

fn split_semicolons_quoted(s: Text) : List(Text)
  var parts = []
  var cur = []
  var in_quotes = false
  var escaped = false
  for ch in s.chars
    if escaped
      cur = cur.append(ch)
      escaped = false
    else if ch == '\\' and in_quotes
      cur = cur.append(ch)
      escaped = true
    else if ch == '"'
      in_quotes = not in_quotes
      cur = cur.append(ch)
    else if ch == ';' and not in_quotes
      parts = parts.append(Text.from_chars(cur))
      cur = []
    else
      cur = cur.append(ch)
    end
  end
  parts.append(Text.from_chars(cur))
end

fn extract_boundary(ct: Text) : Result(Text, FormError)
  segs = split_semicolons_quoted(ct)
  head = (segs.first or "").trim.ascii_lowercase
  return Error(InvalidContentType) if head != "multipart/form-data"
  for seg in segs.drop(1)
    trimmed = seg.trim
    continue if not trimmed.contains?("=")
    pieces = trimmed.split("=")
    pname = (pieces.first or "").trim.ascii_lowercase
    if pname == "boundary"
      pval_raw = pieces.drop(1).join("=")
      b = try unquote_param(pval_raw).or_fail(
        MalformedMultipart(reason: "malformed quoted boundary parameter")
      )
      return Error(MalformedMultipart(reason: "empty multipart boundary")) if b.empty?
      if b.byte_count > max_boundary_bytes or b.contains?("\r") or b.contains?("\n")
        return Error(MalformedMultipart(reason: "invalid multipart boundary"))
      end
      return Ok(b)
    end
  end
  Error(InvalidContentType)
end

fn find_bytes(haystack: Bytes, needle: Bytes, start_at: Int) : Option(Int)
  m = needle.size
  n = haystack.size
  return Some(start_at) if m == 0 and start_at <= n
  return None if m == 0 or start_at < 0 or start_at + m > n
  first_b = try needle.get(0)
  limit = n - m
  var i = start_at
  while i <= limit
    if haystack.get(i) == Some(first_b)
      if haystack.slice(i, i + m) == needle
        return Some(i)
      end
    end
    i += 1
  end
  None
end

fn boundary_suffix(body: Bytes, after_pos: Int) : Option(Bool)
  return None if after_pos + 2 > body.size
  pair = body.slice(after_pos, after_pos + 2)
  if pair == "\r\n".bytes
    Some(false)
  else if pair == "--".bytes
    tail = body.slice(after_pos + 2, body.size)
    if tail.size == 0 or tail == "\r\n".bytes
      Some(true)
    else
      case tail.decode_utf8
        Some(s): if s.trim.empty?: Some(true) else: None
        None: None
      end
    end
  else
    None
  end
end

fn find_next_boundary(body: Bytes, crlf_dash_boundary: Bytes, search_from: Int) : Option(
  (Int, Bool)
)
  var pos = search_from
  m = crlf_dash_boundary.size
  while pos + m <= body.size
    hit = try find_bytes(body, crlf_dash_boundary, pos)
    case boundary_suffix(body, hit + m)
      Some(closing?): return Some((hit, closing?))
      None: pos = hit + 1
    end
  end
  None
end

fn sanitize_filename(raw: Text) : Result(Text, FormError)
  return Error(MalformedMultipart(reason: "filename contains NUL")) if raw.contains?("\u{0}")
  slash_last = raw.split("/").last or raw
  clean = slash_last.split("\\").last or slash_last
  Ok(clean)
end

fn parse_disposition(val: Text) : Result((Text, Option(Text)), FormError)
  segs = split_semicolons_quoted(val)
  disp_type = (segs.first or "").trim.ascii_lowercase
  if disp_type != "form-data"
    return Error(MalformedMultipart(reason: "Content-Disposition must be form-data"))
  end
  var field_name = None
  var fname = None
  for seg in segs.drop(1)
    trimmed = seg.trim
    continue if not trimmed.contains?("=")
    pieces = trimmed.split("=")
    k = (pieces.first or "").trim.ascii_lowercase
    v_raw = pieces.drop(1).join("=")
    v = try unquote_param(v_raw).or_fail(
      MalformedMultipart(reason: "malformed Content-Disposition parameter")
    )
    if k == "name"
      field_name = Some(v)
    else if k == "filename"
      clean_fn = try sanitize_filename(v)
      fname = Some(clean_fn)
    end
  end
  case field_name
    Some(n):
      if n.empty?
        Error(MalformedMultipart(reason: "empty Content-Disposition field name"))
      else
        Ok((n, fname))
      end
    None: Error(MalformedMultipart(reason: "missing Content-Disposition name"))
  end
end

fn parse_part_headers(header_block: Text) : Result((Text, Option(Text), Option(Text)), FormError)
  var disp = None
  var part_ct = None
  for line in header_block.split("\r\n")
    continue if line.empty?
    if not line.contains?(":")
      return Error(MalformedMultipart(reason: "malformed part header line"))
    end
    pieces = line.split(":")
    hname = (pieces.first or "").trim.ascii_lowercase
    hval = pieces.drop(1).join(":").trim
    return Error(MalformedMultipart(reason: "NUL in part header")) if hval.contains?("\u{0}")
    if hname == "content-disposition"
      disp = Some(try parse_disposition(hval))
    else if hname == "content-type"
      part_ct = Some(hval)
    end
  end
  case disp
    Some((field_name, fname)): Ok((field_name, fname, part_ct))
    None: Error(MalformedMultipart(reason: "part missing Content-Disposition"))
  end
end

## Parses a bounded `multipart/form-data` request body (up to `max_bytes` and 128 parts),
## preserving exact binary payloads for uploaded files.
fn parse_multipart(req: Api.Request, max_bytes: Int) : Result(MultipartForm, FormError)
  return Error(BodyTooLarge) if max_bytes < 0 or req.body.size > max_bytes
  ct = req.header("Content-Type") or ""
  boundary = try extract_boundary(ct)

  body = req.body
  open_delim = "--#{boundary}".bytes
  crlf_delim = "\r\n--#{boundary}".bytes
  crlf_crlf = "\r\n\r\n".bytes

  var cursor = 0
  var done? = false
  if body.size >= open_delim.size and body.slice(0, open_delim.size) == open_delim
    case boundary_suffix(body, open_delim.size)
      Some(closing?):
        done? = closing?
        cursor = open_delim.size + 2
      None: return Error(MalformedMultipart(reason: "invalid opening boundary"))
    end
  else
    case find_next_boundary(body, crlf_delim, 0)
      Some((pos, closing?)):
        done? = closing?
        cursor = pos + crlf_delim.size + 2
      None: return Error(MalformedMultipart(reason: "missing opening multipart boundary"))
    end
  end

  var fields = {}
  var files = []
  var part_count = 0

  while not done?
    part_count += 1
    return Error(TooManyParts) if part_count > max_multipart_parts

    next_b = try find_next_boundary(body, crlf_delim, cursor).or_fail(
      MalformedMultipart(reason: "unterminated multipart part")
    )
    (part_end, next_closing?) = next_b
    hdr_end = try find_bytes(body, crlf_crlf, cursor).or_fail(
      MalformedMultipart(reason: "missing CRLFCRLF in multipart part")
    )
    if hdr_end > part_end
      return Error(MalformedMultipart(reason: "missing CRLFCRLF before next boundary"))
    end

    hdr_text = try body.slice(cursor, hdr_end).decode_utf8.or_fail(InvalidUtf8)
    (field_name, maybe_filename, maybe_ct) = try parse_part_headers(hdr_text)
    payload = body.slice(hdr_end + 4, part_end)

    case maybe_filename
      Some(fname): files = files.append(
        UploadedFile(
          field_name: field_name,
          filename: fname,
          content_type: maybe_ct or "application/octet-stream",
          data: payload
        )
      )
      None:
        val_text = try payload.decode_utf8.or_fail(InvalidUtf8)
        fields = append_field(fields, field_name, val_text)
    end

    done? = next_closing?
    cursor = part_end + crlf_delim.size + 2
  end

  Ok(MultipartForm(fields: fields, files: files))
end

test "parse_urlencoded decodes repeated fields, plus-spaces, UTF-8, and rejects bad inputs"
  req = Api.post("/login").with_bytes(
    "email=ada%40example.com&note=hello+world&tag=a&tag=b&unicode=caf%C3%A9".bytes,
    "application/x-www-form-urlencoded; charset=utf-8"
  )
  case parse_urlencoded(req)
    Error(_): assert false
    Ok(fields):
      assert field(fields, "email") == Some("ada@example.com")
      assert field(fields, "note") == Some("hello world")
      assert fields.get("tag") == Some(["a", "b"])
      assert field(fields, "unicode") == Some("café")
  end

  assert parse_urlencoded(Api.post("/login").with_text("a=1")) == Error(InvalidContentType)
  bad_pct = Api.post("/login").with_bytes("a=%ZZ".bytes, "application/x-www-form-urlencoded")
  assert parse_urlencoded(bad_pct) == Error(InvalidUtf8)
end

test "parse_multipart handles quoted boundaries, binary payloads, and malformed inputs"
  bin_payload = case Bytes.from_hex("00fffe8041420d0a2d2d4e6f74426f756e64617279")
    Some(b): b
    None: "".bytes
  end
  part1 = "--my boundary 123\r\nContent-Disposition: form-data; name=\"account_id\"\r\n\r\nchk_01\r\n"
    .bytes
  part2_head = "--my boundary 123\r\nContent-Disposition: form-data; name=\"statement\"; filename=\"C:\\fakepath\\nov.csv\"\r\nContent-Type: text/csv\r\n\r\n"
    .bytes
  part2_body = "date,payee,amount\n2026-11-21,Coffee,-4.50\n".bytes
  part3_head = "\r\n--my boundary 123\r\nContent-Disposition: form-data; name=\"raw_bin\"; filename=\"blob.bin\"\r\n\r\n"
    .bytes
  closing = "\r\n--my boundary 123--\r\n".bytes
  full_body = Bytes.join([part1, part2_head, part2_body, part3_head, bin_payload, closing])

  req = Api.post("/imports").with_bytes(
    full_body,
    "multipart/form-data; boundary=\"my boundary 123\""
  )
  case parse_multipart(req, 65536)
    Error(_): assert false
    Ok(mp):
      assert field(mp.fields, "account_id") == Some("chk_01")
      assert mp.files.size == 2
      csv = file(mp, "statement") or
        UploadedFile(field_name: "", filename: "", content_type: "", data: "".bytes)
      assert csv.filename == "nov.csv"
      assert csv.content_type == "text/csv"
      assert csv.data == part2_body

      bin = file(mp, "raw_bin") or
        UploadedFile(field_name: "", filename: "", content_type: "", data: "".bytes)
      assert bin.filename == "blob.bin"
      assert bin.content_type == "application/octet-stream"
      assert bin.data == bin_payload
  end

  assert parse_multipart(req, 10) == Error(BodyTooLarge)
  assert parse_multipart(Api.post("/imports").with_text("hi"), 1024) == Error(InvalidContentType)
  assert parse_multipart(
    Api.post("/imports").with_bytes(
      "--b\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\nunterminated".bytes,
      "multipart/form-data; boundary=b"
    ),
    1024
  ) is
    Error(MalformedMultipart(reason: _))
  assert parse_multipart(
    Api.post("/imports").with_bytes(
      "--b\r\nContent-Type: text/plain\r\n\r\nno-disp\r\n--b--\r\n".bytes,
      "multipart/form-data; boundary=b"
    ),
    1024
  ) is
    Error(MalformedMultipart(reason: _))
end
