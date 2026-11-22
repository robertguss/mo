## Typed row decoding, write metadata and transaction helpers for Sqlite.
module Sqlite
expose ExecResult(..),
SqliteDecodeError(..),
execute,
transaction,
decode,
decode_one,
decode_optional,
to_text
use Date
use Json

## Write metadata returned by `db.execute`.
struct ExecResult
  changes: Int
  last_insert_rowid: Int
end

## Why decoding `Sqlite.Rows` into a typed struct failed.
enum SqliteDecodeError
  MissingColumn(name: Text)
  TypeMismatch(column: Text, expected: Text, got: Text)
  InvalidValue(column: Text, reason: Text)
  ExpectedOneRow(got: Int)
end

## The decode error as a readable message.
fn to_text(error: SqliteDecodeError) : Text
  examples
    MissingColumn(name: "email").to_text == "missing SQL column 'email'"
    ExpectedOneRow(got: 0).to_text == "expected 1 SQL row, got 0"
  end

  case error
    MissingColumn(name: n): "missing SQL column '#{n}'"
    TypeMismatch(column: c, expected: exp, got: got): "column '#{c}': expected #{exp}, got #{got}"
    InvalidValue(column: c, reason: r): "column '#{c}': #{r}"
    ExpectedOneRow(got: n): "expected 1 SQL row, got #{n}"
  end
end

## Runs a write statement and returns affected row count and `last_insert_rowid`.
fn execute(db: Sqlite.Database, statement: Text, values: List(Sqlite.Value)) : Result(
  ExecResult,
  SqliteError
)
  case db.execute_raw(statement, values)
    Ok((changes, last_id)): Ok(ExecResult(changes: changes, last_insert_rowid: last_id))
    Error(err): Error(err)
  end
end

## Runs `work` inside a `BEGIN IMMEDIATE` transaction, committing on `Ok` and rolling back on `Error`.
fn transaction(
  db: Sqlite.Database,
  ctx: C,
  work: fn(Sqlite.Database, C) : Result(T, E),
  on_sql: fn(SqliteError) : E
) : Result(T, E)
  case db.run("BEGIN IMMEDIATE", [])
    Error(sql_err): return Error(on_sql(sql_err))
    Ok(_): Unit
  end
  case work(db, ctx)
    Ok(val):
      case db.run("COMMIT", [])
        Ok(_): Ok(val)
        Error(sql_err):
          _rb = db.run("ROLLBACK", [])
          Error(on_sql(sql_err))
      end
    Error(app_err):
      _rb = db.run("ROLLBACK", [])
      Error(app_err)
  end
end

## Decodes every row in `rows` into a struct `T` by matching SQL column names to field names.
fn decode(rows: Sqlite.Rows) : Result(List(T), SqliteDecodeError)
  if rows.columns.empty?
    stop "compiler-lowered Sqlite.decode"
  else
    stop "compiler-lowered Sqlite.decode"
  end
end

## Decodes the single row in `rows` into a struct `T`, or returns `ExpectedOneRow`.
fn decode_one(rows: Sqlite.Rows) : Result(T, SqliteDecodeError)
  if rows.columns.empty?
    stop "compiler-lowered Sqlite.decode_one"
  else
    stop "compiler-lowered Sqlite.decode_one"
  end
end

## Decodes zero or one row in `rows` into `Option(T)`, or returns `ExpectedOneRow` when more than one.
fn decode_optional(rows: Sqlite.Rows) : Result(Option(T), SqliteDecodeError)
  if rows.columns.empty?
    stop "compiler-lowered Sqlite.decode_optional"
  else
    stop "compiler-lowered Sqlite.decode_optional"
  end
end

fn find_column_index(columns: List(Text), target: Text) : Option(Int)
  for (i, col) in columns.enumerate
    return Some(i) if col == target
  end
  None
end

fn resolve_columns(columns: List(Text), required: List(Text)) : Result(List(Int), SqliteDecodeError)
  var indices = []
  for name in required
    case find_column_index(columns, name)
      Some(idx): indices = indices.append(idx)
      None: return Error(MissingColumn(name: name))
    end
  end
  Ok(indices)
end

fn row_cell(row: List(Sqlite.Value), indices: List(Int), pos: Int) : Sqlite.Value
  col_idx = indices.get(pos) or -1
  return Sqlite.Null if col_idx < 0
  row.get(col_idx) or Sqlite.Null
end

fn cell_is_null?(v: Sqlite.Value) : Bool
  v is Sqlite.Null
end

fn sqlite_value_kind(v: Sqlite.Value) : Text
  case v
    Sqlite.Null: "NULL"
    Sqlite.Integer(_): "INTEGER"
    Sqlite.Real(_): "REAL"
    Sqlite.String(_): "TEXT"
    Sqlite.Blob(_): "BLOB"
  end
end

fn decode_cell_int(v: Sqlite.Value, col: Text) : Result(Int, SqliteDecodeError)
  case v
    Sqlite.Integer(n): Ok(n)
    Sqlite.Null | Sqlite.Real(_) | Sqlite.String(_) | Sqlite.Blob(_): Error(
      TypeMismatch(column: col, expected: "INTEGER", got: sqlite_value_kind(v))
    )
  end
end

fn decode_cell_bool(v: Sqlite.Value, col: Text) : Result(Bool, SqliteDecodeError)
  case v
    Sqlite.Integer(0): Ok(false)
    Sqlite.Integer(1): Ok(true)
    Sqlite.Integer(n): Error(
      InvalidValue(column: col, reason: "expected 0 or 1 for Bool, got #{n}")
    )
    Sqlite.Null | Sqlite.Real(_) | Sqlite.String(_) | Sqlite.Blob(_): Error(
      TypeMismatch(column: col, expected: "INTEGER", got: sqlite_value_kind(v))
    )
  end
end

fn decode_cell_float(v: Sqlite.Value, col: Text) : Result(Float, SqliteDecodeError)
  case v
    Sqlite.Real(f): Ok(f)
    Sqlite.Integer(n): Ok(n.to_float)
    Sqlite.Null | Sqlite.String(_) | Sqlite.Blob(_): Error(
      TypeMismatch(column: col, expected: "REAL", got: sqlite_value_kind(v))
    )
  end
end

fn decode_cell_text(v: Sqlite.Value, col: Text) : Result(Text, SqliteDecodeError)
  case v
    Sqlite.String(s): Ok(s)
    Sqlite.Null | Sqlite.Integer(_) | Sqlite.Real(_) | Sqlite.Blob(_): Error(
      TypeMismatch(column: col, expected: "TEXT", got: sqlite_value_kind(v))
    )
  end
end

fn decode_cell_bytes(v: Sqlite.Value, col: Text) : Result(Bytes, SqliteDecodeError)
  case v
    Sqlite.Blob(b): Ok(b)
    Sqlite.Null | Sqlite.Integer(_) | Sqlite.Real(_) | Sqlite.String(_): Error(
      TypeMismatch(column: col, expected: "BLOB", got: sqlite_value_kind(v))
    )
  end
end

fn decode_cell_decimal(v: Sqlite.Value, col: Text) : Result(Decimal, SqliteDecodeError)
  case v
    Sqlite.String(s):
      case s.to_decimal
        Some(d): Ok(d)
        None: Error(InvalidValue(column: col, reason: "invalid Decimal: #{s}"))
      end
    Sqlite.Null | Sqlite.Integer(_) | Sqlite.Real(_) | Sqlite.Blob(_): Error(
      TypeMismatch(column: col, expected: "TEXT", got: sqlite_value_kind(v))
    )
  end
end

fn decode_cell_time(v: Sqlite.Value, col: Text) : Result(Time, SqliteDecodeError)
  case v
    Sqlite.String(s):
      case Time.parse(s)
        Some(t): Ok(t)
        None: Error(InvalidValue(column: col, reason: "invalid RFC 3339 Time: #{s}"))
      end
    Sqlite.Null | Sqlite.Integer(_) | Sqlite.Real(_) | Sqlite.Blob(_): Error(
      TypeMismatch(column: col, expected: "TEXT", got: sqlite_value_kind(v))
    )
  end
end

fn decode_cell_date(v: Sqlite.Value, col: Text) : Result(Date.Date, SqliteDecodeError)
  case v
    Sqlite.String(s):
      case Date.parse_iso(s)
        Some(d): Ok(d)
        None: Error(InvalidValue(column: col, reason: "invalid ISO Date: #{s}"))
      end
    Sqlite.Null | Sqlite.Integer(_) | Sqlite.Real(_) | Sqlite.Blob(_): Error(
      TypeMismatch(column: col, expected: "TEXT", got: sqlite_value_kind(v))
    )
  end
end

fn map_json_decode_result(res: Result(A, Json.JsonDecodeError), col: Text) : Result(
  A,
  SqliteDecodeError
)
  case res
    Ok(v): Ok(v)
    Error(e): Error(InvalidValue(column: col, reason: e.to_text))
  end
end

fn decode_rows_driver(
  rows: Sqlite.Rows,
  required: List(Text),
  decode_row: fn(List(Int), List(Sqlite.Value)) : Result(T, SqliteDecodeError)
) : Result(List(T), SqliteDecodeError)
  return Ok([]) if rows.rows.empty? and rows.columns.empty?
  indices = try resolve_columns(rows.columns, required)
  var out = []
  for r in rows.rows
    item = try decode_row(indices, r)
    out = out.append(item)
  end
  Ok(out)
end

fn decode_one_driver(
  rows: Sqlite.Rows,
  required: List(Text),
  decode_row: fn(List(Int), List(Sqlite.Value)) : Result(T, SqliteDecodeError)
) : Result(T, SqliteDecodeError)
  return Error(ExpectedOneRow(got: rows.rows.size)) if rows.rows.size != 1
  indices = try resolve_columns(rows.columns, required)
  first_row = rows.rows.get(0) or []
  decode_row(indices, first_row)
end

fn decode_optional_driver(
  rows: Sqlite.Rows,
  required: List(Text),
  decode_row: fn(List(Int), List(Sqlite.Value)) : Result(T, SqliteDecodeError)
) : Result(Option(T), SqliteDecodeError)
  return Error(ExpectedOneRow(got: rows.rows.size)) if rows.rows.size > 1
  if rows.rows.empty?
    if not rows.columns.empty?
      _cols = try resolve_columns(rows.columns, required)
    end
    return Ok(None)
  end
  indices = try resolve_columns(rows.columns, required)
  first_row = rows.rows.get(0) or []
  val = try decode_row(indices, first_row)
  Ok(Some(val))
end

enum TestStatus
  Draft
  Scheduled(day: Int)
end

struct TestMeta
  tags: List(Text)
  priority: Int
  status: TestStatus
end

struct TestBill
  id: Int
  paid: Bool
  ratio: Float
  title: Text
  digest: Bytes
  amount: Decimal
  created_at: Time
  due_date: Date.Date
  notes: Option(Text)
  meta: TestMeta
end

enum TestAppErr
  DbErr(Text)
  InsufficientFunds
end

fn must_ok(res: Result(A, B)) : A
  case res
    Ok(v): v
    Error(_): stop "unexpected Error"
  end
end

fn transfer_step(db: Sqlite.Database, delta: Int) : Result(Int, TestAppErr)
  return Error(InsufficientFunds) if delta < 0
  ins = try db.execute("INSERT INTO ledger(amount) VALUES (?)", [Sqlite.Integer(delta)]).map_error(
    fn(e) DbErr("#{e}") end
  )
  Ok(ins.last_insert_rowid)
end

test "db.execute, foreign keys, typed row decoding, and transactions"
  sql = Sqlite.in_memory()
  db = must_ok(sql.open("app.db"))

  _s1 = must_ok(
    db.run("CREATE TABLE accounts(id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL)", [])
  )
  _s2 = must_ok(
    db.run(
      "CREATE TABLE bills(id INTEGER PRIMARY KEY AUTOINCREMENT, account_id INTEGER NOT NULL REFERENCES accounts(id), paid INTEGER NOT NULL, ratio REAL NOT NULL, title TEXT NOT NULL, digest BLOB NOT NULL, amount TEXT NOT NULL, created_at TEXT NOT NULL, due_date TEXT NOT NULL, notes TEXT, meta TEXT NOT NULL)",
      []
    )
  )

  meta1 = TestMeta(tags: ["home", "monthly"], priority: 2, status: Scheduled(day: 15))
  meta2 = TestMeta(tags: [], priority: 1, status: Draft)

  fk_err = db.execute(
    "INSERT INTO bills(account_id, paid, ratio, title, digest, amount, created_at, due_date, notes, meta) VALUES (999, 1, 0.5, 'bad', X'CAFE', '12.50', '2026-04-19T12:00:00Z', '2026-05-01', NULL, ?)",
    [Sqlite.String(Json.encode(meta2))]
  )
  assert fk_err is Error(Sqlite.SqlFailed(message: _))

  acc1 = must_ok(db.execute("INSERT INTO accounts(name) VALUES (?)", [Sqlite.String("Utilities")]))
  assert acc1 == ExecResult(changes: 1, last_insert_rowid: 1)

  b1 = must_ok(
    db.execute(
      "INSERT INTO bills(account_id, paid, ratio, title, digest, amount, created_at, due_date, notes, meta) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
      [
        Sqlite.Integer(acc1.last_insert_rowid),
        Sqlite.Integer(1),
        Sqlite.Real(0.75),
        Sqlite.String("Electric"),
        Sqlite.Blob("ab".bytes),
        Sqlite.String("149.95"),
        Sqlite.String("2026-04-19T08:30:00Z"),
        Sqlite.String("2026-05-01"),
        Sqlite.Null,
        Sqlite.String(Json.encode(meta1))
      ]
    )
  )
  assert b1 == ExecResult(changes: 1, last_insert_rowid: 1)

  rows = must_ok(
    db.query(
      "SELECT notes, meta, due_date, created_at, amount, digest, title, ratio, paid, id FROM bills",
      []
    )
  )
  decoded = must_ok(Sqlite.decode[TestBill](rows))
  assert decoded.size == 1
  bill = must_ok(Sqlite.decode_one[TestBill](rows))
  assert bill.id == 1
  assert bill.paid
  assert bill.ratio == 0.75
  assert bill.title == "Electric"
  assert bill.digest == "ab".bytes
  assert bill.amount == 149.95
  assert bill.due_date == (Date.from_ymd(2026, 5, 1) or stop "date")
  assert bill.notes == None
  assert bill.meta == TestMeta(tags: ["home", "monthly"], priority: 2, status: Scheduled(day: 15))
  assert Sqlite.decode_optional[TestBill](rows) == Ok(Some(bill))

  b2 = must_ok(
    db.execute(
      "INSERT INTO bills(account_id, paid, ratio, title, digest, amount, created_at, due_date, notes, meta) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
      [
        Sqlite.Integer(acc1.last_insert_rowid),
        Sqlite.Integer(0),
        Sqlite.Integer(1),
        Sqlite.String("Water"),
        Sqlite.Blob("cd".bytes),
        Sqlite.String("42.10"),
        Sqlite.String("2026-04-20T09:00:00Z"),
        Sqlite.String("2026-05-10"),
        Sqlite.String("autopay"),
        Sqlite.String(Json.encode(meta2))
      ]
    )
  )
  assert b2 == ExecResult(changes: 1, last_insert_rowid: 2)

  upd = must_ok(db.execute("UPDATE bills SET paid = 1", []))
  assert upd.changes == 2

  two_rows = must_ok(db.query("SELECT * FROM bills ORDER BY id", []))
  two_bills = must_ok(Sqlite.decode[TestBill](two_rows))
  assert two_bills.size == 2
  second = two_bills.get(1) or stop "second bill"
  assert second.ratio == 1.0
  assert second.notes == Some("autopay")
  assert second.meta.status == Draft
  assert Sqlite.decode_one[TestBill](two_rows) == Error(ExpectedOneRow(got: 2))
  assert Sqlite.decode_optional[TestBill](two_rows) == Error(ExpectedOneRow(got: 2))

  empty_rows = must_ok(db.query("SELECT * FROM bills WHERE id = 999", []))
  assert Sqlite.decode_optional[TestBill](empty_rows) == Ok(None)
  assert Sqlite.decode_one[TestBill](empty_rows) == Error(ExpectedOneRow(got: 0))

  missing_col = must_ok(db.query("SELECT id FROM bills", []))
  assert Sqlite.decode[TestBill](missing_col) == Error(MissingColumn(name: "paid"))

  cols = [
    "id",
    "paid",
    "ratio",
    "title",
    "digest",
    "amount",
    "created_at",
    "due_date",
    "notes",
    "meta"
  ]
  bad_bool = Sqlite.Rows(
    columns: cols,
    rows: [
      [
        Sqlite.Integer(1),
        Sqlite.Integer(2),
        Sqlite.Integer(1),
        Sqlite.String("x"),
        Sqlite.Blob("".bytes),
        Sqlite.String("1.00"),
        Sqlite.String("2026-04-19T00:00:00Z"),
        Sqlite.String("2026-04-19"),
        Sqlite.String("note"),
        Sqlite.String("{\"tags\":[],\"priority\":0,\"status\":\"Draft\"}")
      ]
    ]
  )
  assert Sqlite.decode_one[TestBill](bad_bool) is Error(InvalidValue(column: "paid", reason: _))

  bad_id = Sqlite.Rows(
    columns: cols,
    rows: [
      [
        Sqlite.String("not-int"),
        Sqlite.Integer(1),
        Sqlite.Integer(1),
        Sqlite.String("x"),
        Sqlite.Blob("".bytes),
        Sqlite.String("1.00"),
        Sqlite.String("2026-04-19T00:00:00Z"),
        Sqlite.String("2026-04-19"),
        Sqlite.Null,
        Sqlite.String("{\"tags\":[],\"priority\":0,\"status\":\"Draft\"}")
      ]
    ]
  )
  assert Sqlite.decode_one[TestBill](bad_id) ==
    Error(TypeMismatch(column: "id", expected: "INTEGER", got: "TEXT"))

  bad_notes = Sqlite.Rows(
    columns: cols,
    rows: [
      [
        Sqlite.Integer(1),
        Sqlite.Integer(1),
        Sqlite.Integer(1),
        Sqlite.String("x"),
        Sqlite.Blob("".bytes),
        Sqlite.String("1.00"),
        Sqlite.String("2026-04-19T00:00:00Z"),
        Sqlite.String("2026-04-19"),
        Sqlite.Integer(42),
        Sqlite.String("{\"tags\":[],\"priority\":0,\"status\":\"Draft\"}")
      ]
    ]
  )
  assert Sqlite.decode_one[TestBill](bad_notes) ==
    Error(TypeMismatch(column: "notes", expected: "TEXT", got: "INTEGER"))

  bad_dec = Sqlite.Rows(
    columns: cols,
    rows: [
      [
        Sqlite.Integer(1),
        Sqlite.Integer(1),
        Sqlite.Integer(1),
        Sqlite.String("x"),
        Sqlite.Blob("".bytes),
        Sqlite.String("not-a-decimal"),
        Sqlite.String("2026-04-19T00:00:00Z"),
        Sqlite.String("2026-04-19"),
        Sqlite.Null,
        Sqlite.String("{\"tags\":[],\"priority\":0,\"status\":\"Draft\"}")
      ]
    ]
  )
  assert Sqlite.decode_one[TestBill](bad_dec) is Error(InvalidValue(column: "amount", reason: _))

  bad_time = Sqlite.Rows(
    columns: cols,
    rows: [
      [
        Sqlite.Integer(1),
        Sqlite.Integer(1),
        Sqlite.Integer(1),
        Sqlite.String("x"),
        Sqlite.Blob("".bytes),
        Sqlite.String("1.00"),
        Sqlite.String("not-a-time"),
        Sqlite.String("2026-04-19"),
        Sqlite.Null,
        Sqlite.String("{\"tags\":[],\"priority\":0,\"status\":\"Draft\"}")
      ]
    ]
  )
  assert Sqlite.decode_one[TestBill](bad_time) is
    Error(InvalidValue(column: "created_at", reason: _))

  bad_date = Sqlite.Rows(
    columns: cols,
    rows: [
      [
        Sqlite.Integer(1),
        Sqlite.Integer(1),
        Sqlite.Integer(1),
        Sqlite.String("x"),
        Sqlite.Blob("".bytes),
        Sqlite.String("1.00"),
        Sqlite.String("2026-04-19T00:00:00Z"),
        Sqlite.String("2026-99-99"),
        Sqlite.Null,
        Sqlite.String("{\"tags\":[],\"priority\":0,\"status\":\"Draft\"}")
      ]
    ]
  )
  assert Sqlite.decode_one[TestBill](bad_date) is Error(InvalidValue(column: "due_date", reason: _))

  bad_json = Sqlite.Rows(
    columns: cols,
    rows: [
      [
        Sqlite.Integer(1),
        Sqlite.Integer(1),
        Sqlite.Integer(1),
        Sqlite.String("x"),
        Sqlite.Blob("".bytes),
        Sqlite.String("1.00"),
        Sqlite.String("2026-04-19T00:00:00Z"),
        Sqlite.String("2026-04-19"),
        Sqlite.Null,
        Sqlite.String("{bad json")
      ]
    ]
  )
  assert Sqlite.decode_one[TestBill](bad_json) is Error(InvalidValue(column: "meta", reason: _))

  _s3 = must_ok(
    db.run("CREATE TABLE ledger(id INTEGER PRIMARY KEY AUTOINCREMENT, amount INTEGER NOT NULL)", [])
  )
  ok_tx = Sqlite.transaction(db, 50, transfer_step, fn(e) DbErr("#{e}") end)
  assert ok_tx == Ok(1)
  rolled_back = Sqlite.transaction(
    db,
    -10,
    fn(tx_db, d)
      _ins = must_ok(tx_db.execute("INSERT INTO ledger(amount) VALUES (999)", []))
      transfer_step(tx_db, d)
    end,
    fn(e) DbErr("#{e}") end
  )
  assert rolled_back == Error(InsufficientFunds)
  ledger_rows = must_ok(db.query("SELECT amount FROM ledger", []))
  assert ledger_rows.rows == [[Sqlite.Integer(50)]]
end
