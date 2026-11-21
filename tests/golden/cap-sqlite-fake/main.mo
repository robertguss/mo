module Main

fn write_and_read(sqlite: Sqlite, path: Text) : Result(Sqlite.Rows, SqliteError)
  db = try sqlite.open(path)
  try db.run("CREATE TABLE t(a INT, b REAL, c TEXT, d BLOB, e INT)", [])
  try db.run(
    "INSERT INTO t VALUES (?, ?, ?, ?, ?)",
    [Sqlite.Integer(7), Sqlite.Real(1.5), Sqlite.String("hi"), Sqlite.Blob("xy".bytes), Sqlite.Null]
  )
  db.query("SELECT a, b, c, d, e FROM t", [])
end

fn escapes?(sqlite: Sqlite, name: Text) : Bool
  promise "a generated sqlite capability rejects escaping paths"
    not result
    allows sqlite: Sqlite.fake(), name: "a.db", result: false
    forbids sqlite: Sqlite.fake(), name: "a.db", result: true
  end

  sqlite.open("../#{name}") is Ok(_)
end

test "a fake sqlite capability stores tables across opens and blocks escapes"
  sqlite = Sqlite.fake()
  expected = Sqlite.Rows(
    columns: ["a", "b", "c", "d", "e"],
    rows: [
      [
        Sqlite.Integer(7),
        Sqlite.Real(1.5),
        Sqlite.String("hi"),
        Sqlite.Blob("xy".bytes),
        Sqlite.Null
      ]
    ]
  )
  assert write_and_read(sqlite, "app.db") == Ok(expected)
  assert sqlite.open("./app.db") is Ok(second)
  assert second.query("SELECT a FROM t", []) ==
    Ok(Sqlite.Rows(columns: ["a"], rows: [[Sqlite.Integer(7)]]))
  assert second.run("ATTACH 'other.db' AS o", []) is Error(Sqlite.NotPermitted(reason: _))
  assert sqlite.open("../escape.db") is Error(Sqlite.OutsideFolder(path: escaped))
  assert escaped == "../escape.db"
end
