## `mox` — human-friendly HTTP/HTTPS client (`httpie`/`xh` in Mo) with shorthand request
## items, formatted output, SQLite request history, and response diffing.
##
##   mo run tools/mox/main.mo [METHOD] URL [ITEM...] [--db PATH] [--color]
##   mo run tools/mox/main.mo history [--db PATH]
##   mo run tools/mox/main.mo diff ID1 ID2 [--db PATH]
module Main
use Http
use Json

default_db = "mox-history.sqlite"

enum RequestItem
  QueryParam(key: Text, value: Text)
  HeaderItem(name: Text, value: Text)
  JsonField(key: Text, raw_json: Text)
  StringField(key: Text, value: Text)
end

enum MoxCommand
  Send(method: Text, url: Text, items: List(RequestItem), db_path: Option(Text), color?: Bool)
  History(db_path: Text)
  Diff(first_id: Int, second_id: Int, db_path: Text)
end

enum MoxError
  Usage(message: Text)
  BadItem(item: Text, reason: Text)
  HttpFailed(message: Text)
  DbFailed(message: Text)
  HistoryNotFound(id: Int)
end

fn error_text(err: MoxError) : Text
  case err
    Usage(message: m): m
    BadItem(item: i, reason: r): "invalid item '#{i}': #{r}"
    HttpFailed(message: m): "http error: #{m}"
    DbFailed(message: m): "sqlite error: #{m}"
    HistoryNotFound(id: id): "history entry ##{id} not found"
  end
end

## Expands httpie/xh URL shorthand (`:8080/items`, `:/health`, `example.com/api`).
fn expand_url(raw: Text) : Text
  promise
    result.starts_with?("http://") or result.starts_with?("https://")
  end

  examples
    expand_url(":8080/items") == "http://localhost:8080/items"
    expand_url(":/health") == "http://localhost/health"
    expand_url("https://api.example.com/v1") == "https://api.example.com/v1"
    expand_url("example.com/v1") == "http://example.com/v1"
  end

  trimmed = raw.trim
  return trimmed if trimmed.starts_with?("http://") or trimmed.starts_with?("https://")
  if trimmed.starts_with?(":/")
    "http://localhost#{trimmed.drop_prefix(":") or ""}"
  else if trimmed.starts_with?(":")
    "http://localhost#{trimmed}"
  else
    "http://#{trimmed}"
  end
end

fn index_of_sub(haystack: Text, needle: Text) : Option(Int)
  return None if needle.empty? or not haystack.contains?(needle)
  parts = haystack.split(needle)
  first = parts.first or ""
  Some(first.byte_count)
end

fn split_once(raw: Text, sep: Text) : Option((Text, Text))
  return None if not raw.contains?(sep)
  parts = raw.split(sep)
  left = parts.first or ""
  right = parts.drop(1).join(sep)
  Some((left, right))
end

## Parses a single httpie/xh shorthand token (`q==val`, `k:=json`, `k=str`, `Header:Val`).
fn parse_item(raw: Text) : Result(RequestItem, MoxError)
  examples
    parse_item("page==2") == Ok(QueryParam(key: "page", value: "2"))
    parse_item("count:=42") == Ok(JsonField(key: "count", raw_json: "42"))
    parse_item("name=Ada") == Ok(StringField(key: "name", value: "Ada"))
    parse_item("Accept:application/json") ==
      Ok(HeaderItem(name: "Accept", value: "application/json"))
  end

  pos_qq = index_of_sub(raw, "==") or 999999
  pos_jeq = index_of_sub(raw, ":=") or 999999
  pos_eq = index_of_sub(raw, "=") or 999999
  pos_col = index_of_sub(raw, ":") or 999999

  if pos_qq <= pos_jeq and pos_qq <= pos_eq and pos_qq <= pos_col and pos_qq < 999999
    (k, v) = try split_once(raw, "==").or_fail(BadItem(item: raw, reason: "malformed query"))
    return Error(BadItem(item: raw, reason: "empty query key")) if k.empty?
    Ok(QueryParam(key: k, value: v))
  else if pos_jeq <= pos_eq and pos_jeq <= pos_col and pos_jeq < 999999
    (k, v) = try split_once(raw, ":=").or_fail(BadItem(item: raw, reason: "malformed := field"))
    return Error(BadItem(item: raw, reason: "empty JSON field key")) if k.empty?
    Ok(JsonField(key: k, raw_json: v))
  else if pos_eq < pos_col and pos_eq < 999999
    (k, v) = try split_once(raw, "=").or_fail(BadItem(item: raw, reason: "malformed = field"))
    return Error(BadItem(item: raw, reason: "empty field key")) if k.empty?
    Ok(StringField(key: k, value: v))
  else if pos_col < 999999
    (k, v) = try split_once(raw, ":").or_fail(BadItem(item: raw, reason: "malformed header"))
    return Error(BadItem(item: raw, reason: "empty header name")) if k.trim.empty?
    Ok(HeaderItem(name: k.trim, value: v.trim))
  else
    Error(BadItem(item: raw, reason: "expected Header:Val, key=val, key:=json, or key==query"))
  end
end

fn http_method_word?(tok: Text) : Bool
  up = tok.ascii_uppercase
  up == "GET" or up == "POST" or up == "PUT" or up == "PATCH" or up == "DELETE" or up == "HEAD"
end

fn has_body_items?(items: List(RequestItem)) : Bool
  items.any?(fn(it)
    case it
      JsonField(key: _, raw_json: _): true
      StringField(key: _, value: _): true
      QueryParam(key: _, value: _) | HeaderItem(name: _, value: _): false
    end
  end)
end

fn parse_send_tail(tokens: List(Text), explicit_method: Option(Text), url: Text) : Result(
  MoxCommand,
  MoxError
)
  var items = []
  var db_path = None
  var color = false
  var idx = 0
  while idx < tokens.size
    tok = tokens.get(idx) or ""
    if tok == "--db"
      next_p = try tokens.get(idx + 1).or_fail(Usage(message: "--db expects a path"))
      db_path = Some(next_p)
      idx += 2
    else if tok == "--color"
      color = true
      idx += 1
    else
      item = try parse_item(tok)
      items = items.append(item)
      idx += 1
    end
  end
  chosen_method = case explicit_method
    Some(m): m
    None: if has_body_items?(items): "POST" else: "GET"
  end
  Ok(
    Send(method: chosen_method, url: expand_url(url), items: items, db_path: db_path, color?: color)
  )
end

fn parse_cli(args: List(Text)) : Result(MoxCommand, MoxError)
  case args
    ["history"]: Ok(History(db_path: default_db))
    ["history", "--db", p] if not p.empty?: Ok(History(db_path: p))
    ["diff", a_raw, b_raw]:
      a = try a_raw.to_int.or_fail(Usage(message: "diff IDs must be integers"))
      b = try b_raw.to_int.or_fail(Usage(message: "diff IDs must be integers"))
      Ok(Diff(first_id: a, second_id: b, db_path: default_db))
    ["diff", a_raw, b_raw, "--db", p] if not p.empty?:
      a = try a_raw.to_int.or_fail(Usage(message: "diff IDs must be integers"))
      b = try b_raw.to_int.or_fail(Usage(message: "diff IDs must be integers"))
      Ok(Diff(first_id: a, second_id: b, db_path: p))
    [first, second, ..rest] if http_method_word?(first): parse_send_tail(
      rest,
      Some(first.ascii_uppercase),
      second
    )
    [only_url] if not only_url.starts_with?("-"): parse_send_tail([], None, only_url)
    [url, ..rest] if not url.starts_with?("-"): parse_send_tail(rest, None, url)
    _: Error(
      Usage(
        message: "usage: mox [METHOD] URL [ITEM...] [--db PATH] | history [--db PATH] | diff ID1 ID2 [--db PATH]"
      )
    )
  end
end

fn build_request(method: Text, url: Text, items: List(RequestItem)) : Result(Http.Request, MoxError)
  var req = Http.request(method, url)
  var fields = {}
  var any_body = false
  for item in items
    case item
      QueryParam(key: k, value: v): req = req.query(k, v)
      HeaderItem(name: n, value: v): req = req.header(n, v)
      StringField(key: k, value: v):
        fields = fields.set(k, Json.String(v))
        any_body = true
      JsonField(key: k, raw_json: raw):
        parsed = try Json.parse(raw).map_error(
          fn(e) BadItem(item: "#{k}:=#{raw}", reason: "invalid JSON at #{e.line}:#{e.column}") end
        )
        fields = fields.set(k, parsed)
        any_body = true
    end
  end
  if any_body
    req = req.json(Json.Object(fields))
  end
  Ok(req)
end

fn format_response(resp: Http.Response, color?: Bool) : Text
  status_str = if color?
    "\u{001b}[1;32mHTTP/1.1 #{resp.status}\u{001b}[0m"
  else
    "HTTP/1.1 #{resp.status}"
  end
  hdr_lines = resp.headers.map(fn(pair) "#{pair.0}: #{pair.1}" end)
  body_txt = resp.text or "<#{resp.body.size} binary bytes>"
  formatted_body = case Json.parse(body_txt)
    Ok(doc): doc.pretty
    Error(_): body_txt
  end
  var lines = [status_str].concat(hdr_lines)
  if not formatted_body.empty?
    lines = lines.append("").append(formatted_body)
  end
  lines.join("\n")
end

fn init_history_db(db: Sqlite.Database) : Result(Unit, MoxError)
  sql = [
    "CREATE TABLE IF NOT EXISTS mox_history (",
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,",
    "  method TEXT NOT NULL,",
    "  url TEXT NOT NULL,",
    "  status INT NOT NULL,",
    "  body TEXT NOT NULL",
    ")"
  ].join("")
  db.run(sql, []).map_error(fn(e) DbFailed(message: "#{e}") end)
end

fn record_exchange(db: Sqlite.Database, method: Text, url: Text, resp: Http.Response) : Result(
  Unit,
  MoxError
)
  try init_history_db(db)
  body_txt = resp.text or ""
  pretty_or_raw = case Json.parse(body_txt)
    Ok(doc): doc.pretty
    Error(_): body_txt
  end
  db.run(
    "INSERT INTO mox_history (method, url, status, body) VALUES (?, ?, ?, ?)",
    [
      Sqlite.String(method),
      Sqlite.String(url),
      Sqlite.Integer(resp.status),
      Sqlite.String(pretty_or_raw)
    ]
  ).map_error(fn(e) DbFailed(message: "#{e}") end)
end

fn cell_str(row: List(Sqlite.Value), idx: Int) : Text
  if row.get(idx) is Some(Sqlite.String(s)): s else: ""
end

fn cell_int(row: List(Sqlite.Value), idx: Int) : Int
  if row.get(idx) is Some(Sqlite.Integer(n)): n else: 0
end

fn load_body(db: Sqlite.Database, id: Int) : Result((Int, Text), MoxError)
  try init_history_db(db)
  rows = try db.query("SELECT status, body FROM mox_history WHERE id = ?", [Sqlite.Integer(id)])
    .map_error(fn(e) DbFailed(message: "#{e}") end)
  row = try rows.rows.first.or_fail(HistoryNotFound(id: id))
  Ok((cell_int(row, 0), cell_str(row, 1)))
end

## Computes line-by-line unified-style diff between two recorded response bodies.
fn diff_bodies(id1: Int, status1: Int, b1: Text, id2: Int, status2: Int, b2: Text) : Text
  var out = ["--- ##{id1} (HTTP #{status1})", "+++ ##{id2} (HTTP #{status2})"]
  lines1 = b1.lines
  lines2 = b2.lines
  set1 = lines1.to_set
  set2 = lines2.to_set
  for line in lines1
    if not set2.has?(line)
      out = out.append("- #{line}")
    end
  end
  for line in lines2
    if not set1.has?(line)
      out = out.append("+ #{line}")
    else
      out = out.append("  #{line}")
    end
  end
  out.join("\n")
end

fn run_cli(args: Args, net: Network, sqlite: Sqlite, io: Console) : Int
  cmd = case parse_cli(args.list)
    Ok(c): c
    Error(e):
      io.error(error_text(e))
      return 2
  end
  case cmd
    Send(method: method, url: url, items: items, db_path: db_path, color?: color?):
      req = case build_request(method, url, items)
        Ok(r): r
        Error(e):
          io.error(error_text(e))
          return 2
      end
      resp = case req.send(net)
        Ok(r): r
        Error(e):
          io.error(error_text(HttpFailed(message: "#{e}")))
          return 1
      end
      case db_path
        Some(p):
          case sqlite.open(p)
            Ok(db):
              case record_exchange(db, method, req.url, resp)
                Ok(_): Unit
                Error(e):
                  io.error(error_text(e))
                  return 1
              end
            Error(e):
              io.error("sqlite error: #{e}")
              return 1
          end
        None: Unit
      end
      io.print(format_response(resp, color?))
      0
    History(db_path: db_path):
      db = case sqlite.open(db_path)
        Ok(d): d
        Error(e):
          io.error("sqlite error: #{e}")
          return 1
      end
      case init_history_db(db)
        Ok(_): Unit
        Error(e):
          io.error(error_text(e))
          return 1
      end
      rows = case db.query("SELECT id, method, url, status FROM mox_history ORDER BY id ASC", [])
        Ok(r): r
        Error(e):
          io.error("sqlite error: #{e}")
          return 1
      end
      for row in rows.rows
        io.print(
          "##{cell_int(row, 0)} #{cell_str(row, 1)} #{cell_str(row, 2)} -> #{cell_int(row, 3)}"
        )
      end
      0
    Diff(first_id: id1, second_id: id2, db_path: db_path):
      db = case sqlite.open(db_path)
        Ok(d): d
        Error(e):
          io.error("sqlite error: #{e}")
          return 1
      end
      (s1, b1) = case load_body(db, id1)
        Ok(pair): pair
        Error(e):
          io.error(error_text(e))
          return 1
      end
      (s2, b2) = case load_body(db, id2)
        Ok(pair): pair
        Error(e):
          io.error(error_text(e))
          return 1
      end
      io.print(diff_bodies(id1, s1, b1, id2, s2, b2))
      0
  end
end

fn main(args: Args, net: Network, sqlite: Sqlite, io: Console) : Int
  run_cli(args, net, sqlite, io)
end

test "httpie shorthand request building, pretty JSON, SQLite history, and response diffing"
  expected_post = Http.post("http://localhost:8080/users").query("dry_run", "true").header(
    "X-Env",
    "staging"
  ).json(Json.Object({"name": Json.String("Ada"), "quota": Json.int(50)}))
  expected_get = Http.get("http://localhost:8080/users")

  net = Network.fake(
    Http.replies([
      (expected_post, Http.reply(201, "{\"id\":1,\"name\":\"Ada\",\"quota\":50}")),
      (expected_get, Http.reply(200, "{\"id\":1,\"name\":\"Ada\",\"quota\":75}"))
    ])
  )
  sqlite = Sqlite.in_memory()

  io1 = Console.fake()
  assert run_cli(
    Args.of([
      ":8080/users",
      "dry_run==true",
      "X-Env:staging",
      "name=Ada",
      "quota:=50",
      "--db",
      "hist.db"
    ]),
    net,
    sqlite,
    io1
  ) ==
    0
  assert (io1.printed.first or "").contains?("HTTP/1.1 201")
  assert (io1.printed.first or "").contains?("  \"quota\": 50")

  io2 = Console.fake()
  assert run_cli(Args.of(["GET", ":8080/users", "--db", "hist.db"]), net, sqlite, io2) == 0

  io_hist = Console.fake()
  assert run_cli(Args.of(["history", "--db", "hist.db"]), Network.fake({}), sqlite, io_hist) == 0
  assert io_hist.printed == [
    "#1 POST http://localhost:8080/users?dry_run=true -> 201",
    "#2 GET http://localhost:8080/users -> 200"
  ]

  io_diff = Console.fake()
  assert run_cli(Args.of(["diff", "1", "2", "--db", "hist.db"]), Network.fake({}), sqlite, io_diff) ==
    0
  diff_out = io_diff.printed.first or ""
  assert diff_out.contains?("--- #1 (HTTP 201)")
  assert diff_out.contains?("+++ #2 (HTTP 200)")
  assert diff_out.contains?("-   \"quota\": 50")
  assert diff_out.contains?("+   \"quota\": 75")
end

test "empty CLI invocation exits 2"
  io = Console.fake()
  assert run_cli(Args.of([]), Network.fake({}), Sqlite.in_memory(), io) == 2
end
