## `moq` — pipe JSON, NDJSON, or CSV to slice by path (`jq`-style, with `[]` fan-out) or
## auto-load into SQLite (`sqlite-utils`-style) for instant SQL analytics.
##
##   mo run tools/moq/main.mo path STEPS [--raw] [--file PATH]
##   mo run tools/moq/main.mo sql QUERY [--table NAME] [--csv] [--format table|json|csv] [--file PATH]
module Main
use Json

enum InputFormat
  AutoJson
  CsvInput
end

enum OutputFormat
  TableOut
  JsonOut
  CsvOut
end

enum MoqCommand
  PathQuery(steps: Text, raw?: Bool, file_path: Option(Text))
  SqlQuery(
    sql: Text,
    table_name: Text,
    input_fmt: InputFormat,
    output_fmt: OutputFormat,
    file_path: Option(Text)
  )
end

enum MoqError
  Usage(message: Text)
  FileFailed(message: Text)
  ParseFailed(message: Text)
  SqlFailed(message: Text)
end

fn error_text(err: MoqError) : Text
  case err
    Usage(message: m): m
    FileFailed(message: m): "file error: #{m}"
    ParseFailed(message: m): "parse error: #{m}"
    SqlFailed(message: m): "sqlite error: #{m}"
  end
end

## Parses a single RFC 4180 CSV line with quoted fields and escaped `""` quotes.
fn parse_csv_line(line: Text) : List(Text)
  promise
    not result.empty?
  end

  examples
    parse_csv_line("ada,eng,180000") == ["ada", "eng", "180000"]
    parse_csv_line("\"Hopper, Grace\",compiler,\"say \"\"hi\"\"\"") == [
      "Hopper, Grace",
      "compiler",
      "say \"hi\""
    ]
  end

  chars = line.chars
  var fields = []
  var cur = []
  var in_quotes = false
  var i = 0
  while i < chars.size
    ch = chars.get(i) or ' '
    if in_quotes
      if ch == '"'
        if (chars.get(i + 1) or ' ') == '"' and i + 1 < chars.size
          cur = cur.append('"')
          i += 2
        else
          in_quotes = false
          i += 1
        end
      else
        cur = cur.append(ch)
        i += 1
      end
    else
      if ch == '"' and cur.empty?
        in_quotes = true
        i += 1
      else if ch == ','
        fields = fields.append(Text.from_chars(cur))
        cur = []
        i += 1
      else
        cur = cur.append(ch)
        i += 1
      end
    end
  end
  fields.append(Text.from_chars(cur))
end

fn csv_escape(cell: Text) : Text
  if cell.contains?(",") or cell.contains?("\"") or cell.contains?("\n")
    "\"#{cell.replace("\"", "\"\"")}\""
  else
    cell
  end
end

## Evaluates a dot-separated path with `[]` array fan-out over a Json value.
fn eval_path(doc: Json, steps: List(Text)) : List(Json)
  case steps
    []: [doc]
    [head, ..tail]:
      if head == "" or head == "."
        eval_path(doc, tail)
      else if head == "[]"
        case doc
          Json.Array(items): items.flat_map(fn(item) eval_path(item, tail) end)
          Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Object(_): []
        end
      else
        case doc
          Json.Object(fields):
            case fields.get(head)
              Some(child): eval_path(child, tail)
              None: []
            end
          Json.Array(items):
            case head.to_int
              Some(idx):
                case items.get(idx)
                  Some(child): eval_path(child, tail)
                  None: []
                end
              None: []
            end
          Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_): []
        end
      end
  end
end

fn read_all_input(io: Console, files: ReadFiles, file_path: Option(Text)) : Result(Text, MoqError)
  case file_path
    Some(p): files.read_text(p).map_error(fn(e) FileFailed(message: "#{e}") end)
    None:
      var lines = []
      while true
        case io.read_line
          Some(line): lines = lines.append(line)
          None: break
        end
      end
      Ok(lines.join("\n"))
  end
end

fn parse_json_or_ndjson(raw: Text) : Result(List(Json), MoqError)
  trimmed = raw.trim
  return Ok([]) if trimmed.empty?
  case Json.parse(trimmed)
    Ok(doc): Ok([doc])
    Error(_):
      var docs = []
      for (idx, line) in trimmed.lines.enumerate
        continue if line.trim.empty?
        doc = try Json.parse(line).map_error(
          fn(e) ParseFailed(message: "line #{idx + 1}: #{e.line}:#{e.column} #{e.problem}") end
        )
        docs = docs.append(doc)
      end
      Ok(docs)
  end
end

fn valid_ident?(name: Text) : Bool
  return false if name.empty?
  first = name.chars.first or '0'
  (first.ascii_letter? or first == '_') and
    name.chars.all?(fn(c) c.ascii_letter? or c.ascii_digit? or c == '_' end)
end

fn sanitize_column(raw: Text, fallback_idx: Int) : Text
  cleaned = Text.from_chars(
    raw.trim.chars.map(fn(c) if c.ascii_letter? or c.ascii_digit? or c == '_': c else: '_' end)
  )
  if cleaned.empty? or (cleaned.chars.first or '_').ascii_digit?
    "c_#{fallback_idx}_#{cleaned}"
  else
    cleaned
  end
end

fn json_to_sqlite_val(v: Json) : Sqlite.Value
  case v
    Json.Null: Sqlite.Null
    Json.Boolean(b): Sqlite.Integer(if b: 1 else: 0)
    Json.Number(num):
      case num.to_int
        Some(i): Sqlite.Integer(i)
        None:
          case num.to_float
            Some(f): Sqlite.Real(f)
            None: Sqlite.String(num.to_text)
          end
      end
    Json.String(s): Sqlite.String(s)
    Json.Array(_) | Json.Object(_): Sqlite.String(v.to_text)
  end
end

fn csv_cell_to_sqlite_val(cell: Text) : Sqlite.Value
  trimmed = cell.trim
  return Sqlite.Null if trimmed.empty?
  case trimmed.to_int
    Some(i): Sqlite.Integer(i)
    None:
      case trimmed.to_float
        Some(f): Sqlite.Real(f)
        None: Sqlite.String(cell)
      end
  end
end

fn load_records_into_sqlite(
  db: Sqlite.Database,
  table_name: Text,
  raw: Text,
  input_fmt: InputFormat
) : Result(Unit, MoqError)
  return Error(Usage(message: "invalid SQL table name '#{table_name}'")) if not valid_ident?(
    table_name
  )
  case input_fmt
    CsvInput:
      all_lines = raw.lines.filter(fn(l) not l.trim.empty? end)
      return Error(ParseFailed(message: "CSV input is empty")) if all_lines.empty?
      raw_headers = parse_csv_line(all_lines.first or "")
      cols = raw_headers.enumerate.map(fn(p) sanitize_column(p.1, p.0) end)
      col_defs = cols.map(fn(c) "\"#{c}\"" end).join(", ")
      try db.run("CREATE TABLE \"#{table_name}\" (#{col_defs})", []).map_error(
        fn(e) SqlFailed(message: "#{e}") end
      )
      placeholders = cols.map(fn(_) "?" end).join(", ")
      insert_sql = "INSERT INTO \"#{table_name}\" VALUES (#{placeholders})"
      for line in all_lines.drop(1)
        parsed_cells = parse_csv_line(line)
        vals = cols.enumerate.map(fn(p) csv_cell_to_sqlite_val(parsed_cells.get(p.0) or "") end)
        try db.run(insert_sql, vals).map_error(fn(e) SqlFailed(message: "#{e}") end)
      end
      Ok(Unit)
    AutoJson:
      docs = try parse_json_or_ndjson(raw)
      objects = case docs
        [Json.Array(items)]: items
        _: docs
      end
      var col_names = []
      var seen = Set.empty
      for item in objects
        if item is Json.Object(fields)
          for k in fields.keys
            if not seen.has?(k)
              seen = seen.add(k)
              col_names = col_names.append(k)
            end
          end
        else
          return Error(
            ParseFailed(message: "SQL loader expects JSON objects or an array of objects")
          )
        end
      end
      return Error(ParseFailed(message: "no JSON object columns found")) if col_names.empty?
      safe_cols = col_names.enumerate.map(fn(p) sanitize_column(p.1, p.0) end)
      col_defs = safe_cols.map(fn(c) "\"#{c}\"" end).join(", ")
      try db.run("CREATE TABLE \"#{table_name}\" (#{col_defs})", []).map_error(
        fn(e) SqlFailed(message: "#{e}") end
      )
      placeholders = safe_cols.map(fn(_) "?" end).join(", ")
      insert_sql = "INSERT INTO \"#{table_name}\" VALUES (#{placeholders})"
      for item in objects
        if item is Json.Object(fields)
          vals = col_names.map(fn(k)
            case fields.get(k)
              Some(v): json_to_sqlite_val(v)
              None: Sqlite.Null
            end
          end)
          try db.run(insert_sql, vals).map_error(fn(e) SqlFailed(message: "#{e}") end)
        end
      end
      Ok(Unit)
  end
end

fn sqlite_val_text(v: Sqlite.Value) : Text
  case v
    Sqlite.Null: "null"
    Sqlite.Integer(n): n.to_text
    Sqlite.Real(f): f.to_text
    Sqlite.String(s): s
    Sqlite.Blob(b): "<#{b.size} bytes>"
  end
end

fn sqlite_val_json(v: Sqlite.Value) : Json
  case v
    Sqlite.Null: Json.Null
    Sqlite.Integer(n): Json.int(n)
    Sqlite.Real(f): Json.float(f) or Json.Null
    Sqlite.String(s): Json.String(s)
    Sqlite.Blob(b): Json.String("<#{b.size} bytes>")
  end
end

fn render_rows(res: Sqlite.Rows, fmt: OutputFormat) : Text
  case fmt
    CsvOut:
      head = res.columns.map(csv_escape).join(",")
      body = res.rows.map(fn(r) r.map(fn(c) csv_escape(sqlite_val_text(c)) end).join(",") end)
      [head].concat(body).join("\n")
    JsonOut:
      objs = res.rows.map(fn(r)
        pairs = res.columns.zip(r).map(fn(p) (p.0, sqlite_val_json(p.1)) end)
        Json.Object(pairs.to_map)
      end)
      Json.Array(objs).pretty
    TableOut:
      head = res.columns.join(" | ")
      sep = res.columns.map(fn(c) "-".repeat(if c.char_count < 3: 3 else: c.char_count) end).join(
        "-|-"
      )
      body = res.rows.map(fn(r) r.map(sqlite_val_text).join(" | ") end)
      [head, sep].concat(body).join("\n")
  end
end

fn parse_path_flags(rest: List(Text), steps: Text, raw?: Bool, file_path: Option(Text)) : Result(
  MoqCommand,
  MoqError
)
  case rest
    []: Ok(PathQuery(steps: steps, raw?: raw?, file_path: file_path))
    ["--raw", ..tail]: parse_path_flags(tail, steps, true, file_path)
    ["--file", p, ..tail] if not p.empty?: parse_path_flags(tail, steps, raw?, Some(p))
    _: Error(Usage(message: "usage: moq path STEPS [--raw] [--file PATH]"))
  end
end

fn parse_sql_flags(
  rest: List(Text),
  sql: Text,
  table_name: Text,
  in_fmt: InputFormat,
  out_fmt: OutputFormat,
  file_path: Option(Text)
) : Result(MoqCommand, MoqError)
  case rest
    []: Ok(
      SqlQuery(
        sql: sql,
        table_name: table_name,
        input_fmt: in_fmt,
        output_fmt: out_fmt,
        file_path: file_path
      )
    )
    ["--csv", ..tail]: parse_sql_flags(tail, sql, table_name, CsvInput, out_fmt, file_path)
    ["--table", t, ..tail] if not t.empty?: parse_sql_flags(
      tail,
      sql,
      t,
      in_fmt,
      out_fmt,
      file_path
    )
    ["--file", p, ..tail] if not p.empty?: parse_sql_flags(
      tail,
      sql,
      table_name,
      in_fmt,
      out_fmt,
      Some(p)
    )
    ["--format", f, ..tail]:
      next_out = case f
        "table": TableOut
        "json": JsonOut
        "csv": CsvOut
        _: return Error(Usage(message: "--format must be table, json, or csv"))
      end
      parse_sql_flags(tail, sql, table_name, in_fmt, next_out, file_path)
    _: Error(
      Usage(
        message: "usage: moq sql QUERY [--table NAME] [--csv] [--format table|json|csv] [--file PATH]"
      )
    )
  end
end

fn parse_cli(args: List(Text)) : Result(MoqCommand, MoqError)
  case args
    ["path", steps, ..rest]: parse_path_flags(rest, steps, false, None)
    ["sql", query, ..rest]: parse_sql_flags(rest, query, "data", AutoJson, TableOut, None)
    _: Error(
      Usage(
        message: "usage: moq path STEPS [--raw] [--file PATH] | sql QUERY [--csv] [--format table|json|csv]"
      )
    )
  end
end

fn run_cli(args: Args, files: ReadFiles, sqlite: Sqlite, io: Console) : Int
  cmd = case parse_cli(args.list)
    Ok(c): c
    Error(e):
      io.error(error_text(e))
      return 2
  end
  case cmd
    PathQuery(steps: steps, raw?: raw?, file_path: file_path):
      raw = case read_all_input(io, files, file_path)
        Ok(t): t
        Error(e):
          io.error(error_text(e))
          return 1
      end
      docs = case parse_json_or_ndjson(raw)
        Ok(d): d
        Error(e):
          io.error(error_text(e))
          return 1
      end
      step_list = if steps == "." or steps == "": [] else: steps.split(".")
      for doc in docs
        for hit in eval_path(doc, step_list)
          if raw? and hit is Json.String(s)
            io.print(s)
          else
            io.print(hit.to_text)
          end
        end
      end
      0
    SqlQuery(
      sql: sql,
      table_name: table_name,
      input_fmt: in_fmt,
      output_fmt: out_fmt,
      file_path: file_path
    ):
      raw = case read_all_input(io, files, file_path)
        Ok(t): t
        Error(e):
          io.error(error_text(e))
          return 1
      end
      db = case sqlite.open(":memory:")
        Ok(d): d
        Error(e):
          io.error("sqlite error: #{e}")
          return 1
      end
      case load_records_into_sqlite(db, table_name, raw, in_fmt)
        Ok(_): Unit
        Error(e):
          io.error(error_text(e))
          return 1
      end
      case db.query(sql, [])
        Ok(rows):
          io.print(render_rows(rows, out_fmt))
          0
        Error(e):
          io.error(error_text(SqlFailed(message: "#{e}")))
          1
      end
  end
end

fn main(args: Args, files: ReadFiles, sqlite: Sqlite, io: Console) : Int
  run_cli(args, files, sqlite, io)
end

test "jq-style array fan-out, NDJSON SQLite auto-loader, and CSV SQL analytics"
  json_input = "{\"team\":\"runtime\",\"members\":[{\"login\":\"ada\"},{\"login\":\"grace\"}]}"
  io_jq = Console.fake_input([json_input])
  assert run_cli(
    Args.of(["path", "members.[].login", "--raw"]),
    Files.in_memory({}).read_only,
    Sqlite.in_memory(),
    io_jq
  ) ==
    0
  assert io_jq.printed == ["ada", "grace"]

  ndjson_lines = [
    "{\"dept\":\"compiler\",\"engineer\":\"ada\",\"commits\":42}",
    "{\"dept\":\"compiler\",\"engineer\":\"niklaus\",\"commits\":18}",
    "{\"dept\":\"vm\",\"engineer\":\"grace\",\"commits\":55}"
  ]
  io_sql = Console.fake_input(ndjson_lines)
  assert run_cli(
    Args.of([
      "sql",
      "SELECT dept, COUNT(*) AS n, SUM(commits) AS total FROM data GROUP BY dept ORDER BY total DESC",
      "--format",
      "csv"
    ]),
    Files.in_memory({}).read_only,
    Sqlite.in_memory(),
    io_sql
  ) ==
    0
  assert io_sql.printed == ["dept,n,total\ncompiler,2,60\nvm,1,55"]

  fs = Files.in_memory({
    "sales.csv": "region,rep,amount\nWest,\"Hopper, Grace\",120\nEast,Ada,80\nWest,Linus,30\n"
  }).read_only
  io_csv = Console.fake()
  assert run_cli(
    Args.of([
      "sql",
      "SELECT region, SUM(amount) AS revenue FROM sales WHERE amount >= 50 GROUP BY region ORDER BY revenue DESC",
      "--csv",
      "--table",
      "sales",
      "--file",
      "sales.csv"
    ]),
    fs,
    Sqlite.in_memory(),
    io_csv
  ) ==
    0
  assert io_csv.printed == ["region | revenue\n-------|--------\nWest | 120\nEast | 80"]
end

test "empty CLI arguments exit 2"
  io = Console.fake()
  assert run_cli(Args.of([]), Files.in_memory({}).read_only, Sqlite.in_memory(), io) == 2
end
