## Cursor-paginated Work-API ETL CLI: ingest remote JSON invoices into SQLite and emit
## exact-Decimal revenue reports in Markdown, CSV, or JSON.
##
##   mo run tools/work-etl/main.mo ingest BASE_URL [--db PATH]
##   mo run tools/work-etl/main.mo report [--db PATH] [--format markdown|csv|json] [--out FILE]
module Main
use Http
use Json

default_db = "work-etl.sqlite"

enum InvoiceStatus
  Draft
  Paid
  Void
  Overdue
end

struct Invoice
  id: Text
  customer: Text
  status: InvoiceStatus
  amount: Decimal
  issued_at: Time
end

struct InvoicePage
  items: List(Invoice)
  next_cursor: Option(Text)
end

struct CustomerSummary
  customer: Text
  paid_count: Int
  paid_total: Decimal
  overdue_total: Decimal
end

enum ReportFormat
  Markdown
  Csv
  JsonOut
end

enum EtlCommand
  Ingest(base_url: Text, db_path: Text)
  Report(db_path: Text, format: ReportFormat, out_file: Option(Text))
end

enum EtlError
  Usage(message: Text)
  BadUrl(url: Text)
  HttpFailed(message: Text)
  DecodeFailed(message: Text)
  DbFailed(message: Text)
  FileFailed(message: Text)
end

fn error_text(err: EtlError) : Text
  case err
    Usage(message: m): m
    BadUrl(url: u): "invalid base URL: #{u}"
    HttpFailed(message: m): "http error: #{m}"
    DecodeFailed(message: m): "json decode error: #{m}"
    DbFailed(message: m): "sqlite error: #{m}"
    FileFailed(message: m): "file error: #{m}"
  end
end

fn status_name(s: InvoiceStatus) : Text
  case s
    Draft: "draft"
    Paid: "paid"
    Void: "void"
    Overdue: "overdue"
  end
end

## Extracts authority ("host" or "host:port") from an http:// or https:// base URL.
fn extract_authority(url: Text) : Result((Text, Text), EtlError)
  examples
    extract_authority("https://billing.example.com/v1") ==
      Ok(("billing.example.com:443", "https://billing.example.com/v1"))
    extract_authority("http://localhost:8080/api/") ==
      Ok(("localhost:8080", "http://localhost:8080/api"))
    extract_authority("ftp://bad") == Error(BadUrl(url: "ftp://bad"))
  end

  trimmed = url.trim
  normalized = case trimmed.drop_suffix("/")
    Some(without): without
    None: trimmed
  end
  case normalized.drop_prefix("https://")
    Some(rest):
      host_port = rest.split("/").first or ""
      return Error(BadUrl(url: url)) if host_port.empty?
      authority = if host_port.contains?(":"): host_port else: "#{host_port}:443"
      Ok((authority, normalized))
    None:
      case normalized.drop_prefix("http://")
        Some(rest):
          host_port = rest.split("/").first or ""
          return Error(BadUrl(url: url)) if host_port.empty?
          authority = if host_port.contains?(":"): host_port else: "#{host_port}:80"
          Ok((authority, normalized))
        None: Error(BadUrl(url: url))
      end
  end
end

## Escapes a field for RFC 4180 CSV output.
fn csv_escape(field: Text) : Text
  promise
    (field.contains?(",") or field.contains?("\"") or field.contains?("\n")) implies
      result.starts_with?("\"") and result.ends_with?("\"")
  end

  examples
    csv_escape("Acme") == "Acme"
    csv_escape("Acme, Inc.") == "\"Acme, Inc.\""
    csv_escape("Say \"hi\"") == "\"Say \"\"hi\"\"\""
  end

  if field.contains?(",") or field.contains?("\"") or field.contains?("\n") or field.contains?("\r")
    escaped = field.replace("\"", "\"\"")
    "\"#{escaped}\""
  else
    field
  end
end

## Paid invoice contribution to net collected revenue.
fn paid_contribution(status: InvoiceStatus, amount: Decimal) : Decimal
  promise
    amount >= 0.0 implies result >= 0.0
    status != Paid implies result == 0.0
  end

  if status == Paid and amount > 0.0: amount else: 0.0
end

fn parse_report_flags(rest: List(Text), db_path: Text, fmt: ReportFormat, out_file: Option(Text)) : Result(
  EtlCommand,
  EtlError
)
  case rest
    []: Ok(Report(db_path: db_path, format: fmt, out_file: out_file))
    ["--db", p, ..tail] if not p.empty?: parse_report_flags(tail, p, fmt, out_file)
    ["--out", f, ..tail] if not f.empty?: parse_report_flags(tail, db_path, fmt, Some(f))
    ["--format", raw, ..tail]:
      next_fmt = case raw
        "markdown": Markdown
        "csv": Csv
        "json": JsonOut
        _: return Error(Usage(message: "--format must be markdown, csv, or json"))
      end
      parse_report_flags(tail, db_path, next_fmt, out_file)
    _: Error(
      Usage(message: "usage: work-etl report [--db PATH] [--format markdown|csv|json] [--out FILE]")
    )
  end
end

fn parse_cli(args: List(Text)) : Result(EtlCommand, EtlError)
  case args
    ["ingest", base_url]: Ok(Ingest(base_url: base_url, db_path: default_db))
    ["ingest", base_url, "--db", db_path] if not db_path.empty?: Ok(
      Ingest(base_url: base_url, db_path: db_path)
    )
    ["report", ..rest]: parse_report_flags(rest, default_db, Markdown, None)
    _: Error(
      Usage(
        message: "usage: work-etl ingest BASE_URL [--db PATH] | report [--db PATH] [--format markdown|csv|json]"
      )
    )
  end
end

fn init_schema(db: Sqlite.Database) : Result(Unit, EtlError)
  stmt = [
    "CREATE TABLE IF NOT EXISTS invoices (",
    "  id TEXT PRIMARY KEY,",
    "  customer TEXT NOT NULL,",
    "  status TEXT NOT NULL,",
    "  amount TEXT NOT NULL,",
    "  issued_at TEXT NOT NULL",
    ")"
  ].join("")
  db.run(stmt, []).map_error(fn(e) DbFailed(message: "#{e}") end)
end

fn upsert_invoice(db: Sqlite.Database, inv: Invoice) : Result(Unit, EtlError)
  sql = [
    "INSERT INTO invoices (id, customer, status, amount, issued_at) VALUES (?, ?, ?, ?, ?) ",
    "ON CONFLICT(id) DO UPDATE SET ",
    "  customer = excluded.customer,",
    "  status = excluded.status,",
    "  amount = excluded.amount,",
    "  issued_at = excluded.issued_at"
  ].join("")
  params = [
    Sqlite.String(inv.id),
    Sqlite.String(inv.customer),
    Sqlite.String(status_name(inv.status)),
    Sqlite.String(inv.amount.to_fixed(2)),
    Sqlite.String(inv.issued_at.to_text)
  ]
  db.run(sql, params).map_error(fn(e) DbFailed(message: "#{e}") end)
end

fn build_invoices_request(base_url: Text, cursor: Option(Text), api_key: Option(Text)) : Http
  .Request
  base = Http.get("#{base_url}/invoices").header("Accept", "application/json")
  with_cursor = case cursor
    Some(c): base.query("cursor", c)
    None: base
  end
  case api_key
    Some(k) if not k.trim.empty?: with_cursor.header("Authorization", "Bearer #{k.trim}")
    Some(_) | None: with_cursor
  end
end

fn ingest_all(net: Network, db: Sqlite.Database, raw_url: Text, api_key: Option(Text)) : Result(
  (Int, Int),
  EtlError
)
  (authority, base_url) = try extract_authority(raw_url)
  try init_schema(db)
  scoped = net.allow_hosts([authority])
  var cursor = None
  var pages = 0
  var count = 0
  while pages < 100
    req = build_invoices_request(base_url, cursor, api_key)
    resp = try req.send(scoped).map_error(fn(e) HttpFailed(message: "#{e}") end)
    return Error(HttpFailed(message: "HTTP #{resp.status}")) if not resp.ok?
    body_text = try resp.text.or_fail(DecodeFailed(message: "response is not UTF-8"))
    page = try Json.decode[InvoicePage](body_text).map_error(
      fn(e) DecodeFailed(message: "#{e}") end
    )
    pages += 1
    for inv in page.items
      return Error(DecodeFailed(message: "negative invoice #{inv.id}")) if inv.amount < 0.0
      try upsert_invoice(db, inv)
      count += 1
    end
    case page.next_cursor
      Some(next) if not next.empty?: cursor = Some(next)
      Some(_) | None: break
    end
  end
  Ok((pages, count))
end

fn row_str(row: List(Sqlite.Value), idx: Int) : Text
  if row.get(idx) is Some(Sqlite.String(s)): s else: ""
end

fn load_summaries(db: Sqlite.Database) : Result(List(CustomerSummary), EtlError)
  try init_schema(db)
  rows = try db.query(
    "SELECT customer, status, amount FROM invoices ORDER BY customer ASC, id ASC",
    []
  ).map_error(fn(e) DbFailed(message: "#{e}") end)
  var by_customer = {}
  for row in rows.rows
    cust = row_str(row, 0)
    st = row_str(row, 1)
    amt = row_str(row, 2).to_decimal or 0.0
    prev = by_customer.get(cust) or
      CustomerSummary(customer: cust, paid_count: 0, paid_total: 0.0, overdue_total: 0.0)
    updated = if st == "paid"
      CustomerSummary(
        customer: cust,
        paid_count: prev.paid_count + 1,
        paid_total: prev.paid_total + paid_contribution(Paid, amt),
        overdue_total: prev.overdue_total
      )
    else if st == "overdue"
      CustomerSummary(
        customer: cust,
        paid_count: prev.paid_count,
        paid_total: prev.paid_total,
        overdue_total: prev.overdue_total + amt
      )
    else
      prev
    end
    by_customer = by_customer.set(cust, updated)
  end
  Ok(by_customer.values.sort_by_desc(fn(s) s.paid_total end))
end

fn render_report(items: List(CustomerSummary), fmt: ReportFormat) : Text
  case fmt
    JsonOut: Json.encode_pretty(items)
    Csv:
      header = "customer,paid_count,paid_total,overdue_total"
      lines = items.map(fn(s)
        "#{csv_escape(s.customer)},#{s.paid_count},#{s.paid_total.to_fixed(2)},#{s.overdue_total
          .to_fixed(2)}"
      end)
      [header].concat(lines).join("\n")
    Markdown:
      total_paid = items.map(fn(s) s.paid_total end).sum
      total_overdue = items.map(fn(s) s.overdue_total end).sum
      head = ["| Customer | Paid Invoices | Paid Revenue | Overdue |", "| --- | --- | --- | --- |"]
      body = items.map(fn(s)
        "| #{s.customer} | #{s.paid_count} | $#{s.paid_total.to_fixed(2)} | $#{s.overdue_total
          .to_fixed(2)} |"
      end)
      foot = [
        "| **Total** | | **$#{total_paid.to_fixed(2)}** | **$#{total_overdue.to_fixed(2)}** |"
      ]
      head.concat(body).concat(foot).join("\n")
  end
end

fn run_cli(args: Args, env: Env, net: Network, sqlite: Sqlite, files: Files, io: Console) : Int
  cmd = case parse_cli(args.list)
    Ok(c): c
    Error(e):
      io.error(error_text(e))
      return 2
  end
  case cmd
    Ingest(base_url: base_url, db_path: db_path):
      db = case sqlite.open(db_path)
        Ok(d): d
        Error(e):
          io.error("sqlite error: #{e}")
          return 1
      end
      case ingest_all(net, db, base_url, env.get("WORK_API_KEY"))
        Ok((pages, count)):
          io.print("ingested #{count} invoices across #{pages} pages")
          0
        Error(e):
          io.error(error_text(e))
          1
      end
    Report(db_path: db_path, format: fmt, out_file: out_file):
      db = case sqlite.open(db_path)
        Ok(d): d
        Error(e):
          io.error("sqlite error: #{e}")
          return 1
      end
      summaries = case load_summaries(db)
        Ok(s): s
        Error(e):
          io.error(error_text(e))
          return 1
      end
      rendered = render_report(summaries, fmt)
      case out_file
        Some(path):
          case files.write_text(path, "#{rendered}\n")
            Ok(_):
              io.print("wrote #{path}")
              0
            Error(e):
              io.error(error_text(FileFailed(message: "#{e}")))
              1
          end
        None:
          io.print(rendered)
          0
      end
  end
end

fn main(args: Args, env: Env, net: Network, sqlite: Sqlite, files: Files, io: Console) : Int
  run_cli(args, env, net, sqlite, files, io)
end

test "cursor-paginated ETL ingest, exact Decimal totals, and CSV/Markdown/JSON export"
  base = "https://billing.example.com/v1"
  r1 = build_invoices_request(base, None, Some("wk_secret"))
  r2 = build_invoices_request(base, Some("cur_2"), Some("wk_secret"))
  p1 = [
    "{\"items\":[",
    "{\"id\":\"inv_1\",\"customer\":\"Acme, Inc.\",\"status\":\"paid\",\"amount\":\"199.95\",\"issued_at\":\"2026-11-01T10:00:00Z\"},",
    "{\"id\":\"inv_2\",\"customer\":\"Globex\",\"status\":\"overdue\",\"amount\":\"45.50\",\"issued_at\":\"2026-11-02T10:00:00Z\"}",
    "],\"next_cursor\":\"cur_2\"}"
  ].join("")
  p2 = [
    "{\"items\":[",
    "{\"id\":\"inv_3\",\"customer\":\"Acme, Inc.\",\"status\":\"paid\",\"amount\":\"0.05\",\"issued_at\":\"2026-11-03T10:00:00Z\"},",
    "{\"id\":\"inv_4\",\"customer\":\"Globex\",\"status\":\"void\",\"amount\":\"999.00\",\"issued_at\":\"2026-11-04T10:00:00Z\"},",
    "{\"id\":\"inv_5\",\"customer\":\"Globex\",\"status\":\"paid\",\"amount\":\"80.00\",\"issued_at\":\"2026-11-05T10:00:00Z\"}",
    "],\"next_cursor\":null}"
  ].join("")
  net = Network.fake(Http.replies([(r1, Http.reply(200, p1)), (r2, Http.reply(200, p2))]))
  sqlite = Sqlite.in_memory()
  files = Files.in_memory({})
  env = Env.of({"WORK_API_KEY": "wk_secret"})
  io_in = Console.fake()
  assert run_cli(Args.of(["ingest", base, "--db", "etl.db"]), env, net, sqlite, files, io_in) == 0
  assert io_in.printed == ["ingested 5 invoices across 2 pages"]

  io_md = Console.fake()
  assert run_cli(
    Args.of(["report", "--db", "etl.db", "--format", "markdown"]),
    env,
    Network.fake({}),
    sqlite,
    files,
    io_md
  ) ==
    0
  assert io_md.printed == [
    [
      "| Customer | Paid Invoices | Paid Revenue | Overdue |",
      "| --- | --- | --- | --- |",
      "| Acme, Inc. | 2 | $200.00 | $0.00 |",
      "| Globex | 1 | $80.00 | $45.50 |",
      "| **Total** | | **$280.00** | **$45.50** |"
    ].join("\n")
  ]

  io_csv = Console.fake()
  assert run_cli(
    Args.of(["report", "--db", "etl.db", "--format", "csv", "--out", "report.csv"]),
    env,
    Network.fake({}),
    sqlite,
    files,
    io_csv
  ) ==
    0
  assert files.read_text("report.csv") ==
    Ok(
      [
        "customer,paid_count,paid_total,overdue_total",
        "\"Acme, Inc.\",2,200.00,0.00",
        "Globex,1,80.00,45.50",
        ""
      ].join("\n")
    )
end

test "empty CLI arguments exit 2"
  io = Console.fake()
  assert run_cli(
    Args.of([]),
    Env.of({}),
    Network.fake({}),
    Sqlite.in_memory(),
    Files.in_memory({}),
    io
  ) ==
    2
end
