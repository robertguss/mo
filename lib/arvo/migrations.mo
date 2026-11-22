## Ordered SQLite schema migration runner tracked in `_arvo_migrations` with SHA-256
## checksum verification, transactional application, and trigger-aware SQL statement splitting.
module Arvo.Migrations
expose Migration(..), MigrationError(..), split_statements, run, applied_versions
use Crypto

## One versioned SQL migration script (`version` must be positive and strictly ascending).
struct Migration
  version: Int
  name: Text
  sql: Text
end

## Why running migrations failed.
enum MigrationError
  Sql(SqliteError)
  Decode(Sqlite.SqliteDecodeError)
  ChecksumMismatch(version: Int, name: Text)
  OutOfOrder(version: Int)
end

struct AppliedMigration
  version: Int
  name: Text
  checksum: Text
  applied_at: Time
end

struct ApplyPlan
  migration: Migration
  statements: List(Text)
  checksum: Text
  applied_at_iso: Text
end

tracking_table_ddl = "CREATE TABLE IF NOT EXISTS _arvo_migrations (version INTEGER PRIMARY KEY, name TEXT NOT NULL, checksum TEXT NOT NULL, applied_at TEXT NOT NULL) STRICT"

fn ident_word_char?(ch: Char) : Bool
  ch.ascii_letter? or ch.ascii_digit? or ch == '_'
end

## Splits a SQL migration script into individual executable statements while respecting
## single-quoted strings (`''`), double-quoted identifiers (`""`), `--` line comments,
## `/* ... */` block comments, and `CREATE TRIGGER ... BEGIN ... [CASE ... END] ... END` blocks.
fn split_statements(sql: Text) : List(Text)
  chars = sql.chars
  n = chars.size
  var stmts = []
  var cur = []
  var i = 0
  var in_trigger_header = false
  var block_depth = 0

  while i < n
    ch = chars.get(i) or ' '
    next_ch = chars.get(i + 1) or ' '

    if ch == '-' and next_ch == '-'
      i += 2
      while i < n and (chars.get(i) or '\n') != '\n'
        i += 1
      end
      cur = cur.append('\n')
    else if ch == '/' and next_ch == '*'
      i += 2
      while i < n
        if (chars.get(i) or ' ') == '*' and (chars.get(i + 1) or ' ') == '/'
          i += 2
          break
        end
        i += 1
      end
      cur = cur.append(' ')
    else if ch == '\''
      cur = cur.append('\'')
      i += 1
      while i < n
        sc = chars.get(i) or '\''
        cur = cur.append(sc)
        i += 1
        if sc == '\''
          if i < n and (chars.get(i) or ' ') == '\''
            cur = cur.append('\'')
            i += 1
          else
            break
          end
        end
      end
    else if ch == '"'
      cur = cur.append('"')
      i += 1
      while i < n
        dc = chars.get(i) or '"'
        cur = cur.append(dc)
        i += 1
        if dc == '"'
          if i < n and (chars.get(i) or ' ') == '"'
            cur = cur.append('"')
            i += 1
          else
            break
          end
        end
      end
    else if ch == ';' and block_depth == 0
      stmt = Text.from_chars(cur).trim
      if not stmt.empty?
        stmts = stmts.append(stmt)
      end
      cur = []
      in_trigger_header = false
      i += 1
    else if ch.ascii_letter? or ch == '_'
      var word_chars = []
      while i < n and ident_word_char?(chars.get(i) or ' ')
        wc = chars.get(i) or '_'
        word_chars = word_chars.append(wc)
        cur = cur.append(wc)
        i += 1
      end
      upper = Text.from_chars(word_chars).ascii_uppercase
      if upper == "TRIGGER" and block_depth == 0
        in_trigger_header = true
      else if upper == "BEGIN" and in_trigger_header and block_depth == 0
        in_trigger_header = false
        block_depth = 1
      else if upper == "CASE" and block_depth > 0
        block_depth += 1
      else if upper == "END" and block_depth > 0
        block_depth -= 1
      end
    else
      cur = cur.append(ch)
      i += 1
    end
  end

  tail = Text.from_chars(cur).trim
  if not tail.empty?
    stmts = stmts.append(tail)
  end
  stmts
end

fn migration_checksum(sql: Text) : Text
  Crypto.sha256(sql.bytes).to_hex
end

fn apply_single_tx(tx_db: Sqlite.Database, plan: ApplyPlan) : Result(Unit, MigrationError)
  for stmt in plan.statements
    try tx_db.run(stmt, []).map_error(fn(e) Sql(e) end)
  end
  _ins = try tx_db.execute(
    "INSERT INTO _arvo_migrations (version, name, checksum, applied_at) VALUES (?, ?, ?, ?)",
    [
      Sqlite.Integer(plan.migration.version),
      Sqlite.String(plan.migration.name),
      Sqlite.String(plan.checksum),
      Sqlite.String(plan.applied_at_iso)
    ]
  ).map_error(fn(e) Sql(e) end)
  Ok(Unit)
end

## Returns the ascending list of already-applied migration version numbers in `db`.
fn applied_versions(db: Sqlite.Database) : Result(List(Int), MigrationError)
  try db.run(tracking_table_ddl, []).map_error(fn(e) Sql(e) end)
  rows = try db.query(
    "SELECT version, name, checksum, applied_at FROM _arvo_migrations ORDER BY version ASC",
    []
  ).map_error(fn(e) Sql(e) end)
  applied = try Sqlite.decode[AppliedMigration](rows).map_error(fn(e) Decode(e) end)
  Ok(applied.map(fn(a) a.version end))
end

## Validates `migrations`, verifies SHA-256 checksums of already-applied versions,
## applies pending migrations atomically inside transactions, and returns the count newly applied.
fn run(db: Sqlite.Database, clock: Clock, migrations: List(Migration)) : Result(Int, MigrationError)
  var prev_v = 0
  for m in migrations
    return Error(OutOfOrder(version: m.version)) if m.version <= prev_v
    prev_v = m.version
  end

  try db.run(tracking_table_ddl, []).map_error(fn(e) Sql(e) end)
  rows = try db.query(
    "SELECT version, name, checksum, applied_at FROM _arvo_migrations ORDER BY version ASC",
    []
  ).map_error(fn(e) Sql(e) end)
  applied_list = try Sqlite.decode[AppliedMigration](rows).map_error(fn(e) Decode(e) end)

  var applied_map = {}
  var max_applied = 0
  for row in applied_list
    applied_map = applied_map.set(row.version, row)
    if row.version > max_applied
      max_applied = row.version
    end
  end

  for m in migrations
    case applied_map.get(m.version)
      Some(existing):
        expected = migration_checksum(m.sql)
        if not Crypto.constant_time_eq?(existing.checksum.bytes, expected.bytes)
          return Error(ChecksumMismatch(version: m.version, name: m.name))
        end
      None:
        if m.version < max_applied
          return Error(OutOfOrder(version: m.version))
        end
    end
  end

  var count = 0
  for m in migrations
    if not applied_map.has?(m.version)
      plan = ApplyPlan(
        migration: m,
        statements: split_statements(m.sql),
        checksum: migration_checksum(m.sql),
        applied_at_iso: clock.now.to_text
      )
      _tx = try Sqlite.transaction(db, plan, apply_single_tx, fn(e) Sql(e) end)
      count += 1
    end
  end
  Ok(count)
end

fn must_ok_mig(res: Result(A, B)) : A
  case res
    Ok(v): v
    Error(_): stop "unexpected migration error"
  end
end

test "split_statements handles quotes, comments, and CREATE TRIGGER ... BEGIN ... CASE ... END"
  script = """
    -- Leading comment; with semicolons;
    CREATE TABLE items (
      id INTEGER PRIMARY KEY,
      "weird;col" TEXT NOT NULL DEFAULT 'it''s; fine -- not comment /* no */',
      changes INTEGER NOT NULL DEFAULT 0
    ) STRICT;
    /* Multi-line; block; comment */
    CREATE TRIGGER items_bump AFTER UPDATE ON items
    BEGIN
      UPDATE items
        SET changes = CASE WHEN NEW.changes >= 0 THEN NEW.changes + 1 ELSE 1 END
        WHERE id = NEW.id;
    END;
    """
  stmts = split_statements(script)
  assert stmts.size == 2
  assert (stmts.get(0) or "").starts_with?("CREATE TABLE items")
  assert (stmts.get(1) or "").starts_with?("CREATE TRIGGER items_bump")
end

test "migrations run idempotently, enforce checksums & ordering, and roll back on SQL failure"
  sql = Sqlite.in_memory()
  db = must_ok_mig(sql.open("mig.db"))
  clock = Clock.fixed(Time.parse("2026-11-21T15:00:00Z") or stop "time")

  m1 = Migration(
    version: 1,
    name: "create_notes",
    sql: """
      CREATE TABLE notes (id INTEGER PRIMARY KEY, body TEXT NOT NULL, rev INTEGER NOT NULL DEFAULT 1) STRICT;
      CREATE TABLE note_log (note_id INTEGER NOT NULL, tag TEXT NOT NULL) STRICT;
      CREATE TRIGGER notes_after_insert AFTER INSERT ON notes
      BEGIN
        INSERT INTO note_log (note_id, tag) VALUES (NEW.id, CASE WHEN NEW.rev > 0 THEN 'ok;1' ELSE 'zero' END);
      END;
      """
  )
  m2 = Migration(
    version: 2,
    name: "seed_note",
    sql: "INSERT INTO notes (id, body, rev) VALUES (1, 'hello; world', 1);"
  )

  assert run(db, clock, [m2, m1]) == Error(OutOfOrder(version: 1))
  assert run(db, clock, [Migration(version: 0, name: "zero", sql: "SELECT 1;")]) ==
    Error(OutOfOrder(version: 0))

  assert run(db, clock, [m1]) == Ok(1)
  assert run(db, clock, [m1]) == Ok(0)
  assert run(db, clock, [m1, m2]) == Ok(1)
  assert applied_versions(db) == Ok([1, 2])

  log_rows = must_ok_mig(db.query("SELECT note_id, tag FROM note_log", []))
  assert log_rows.rows == [[Sqlite.Integer(1), Sqlite.String("ok;1")]]

  tampered_m1 = Migration(version: 1, name: "create_notes", sql: "SELECT 1;")
  assert run(db, clock, [tampered_m1, m2]) ==
    Error(ChecksumMismatch(version: 1, name: "create_notes"))

  m3_broken = Migration(
    version: 3,
    name: "partial_failure",
    sql: "INSERT INTO notes (id, body, rev) VALUES (2, 'rolled back', 1); INSERT INTO nonexistent_table VALUES (1);"
  )
  assert run(db, clock, [m1, m2, m3_broken]) is Error(Sql(_))
  assert applied_versions(db) == Ok([1, 2])
  note_rows = must_ok_mig(db.query("SELECT id FROM notes ORDER BY id", []))
  assert note_rows.rows == [[Sqlite.Integer(1)]]
end
