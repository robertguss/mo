module Main

fn describe(error: SqliteError) : Text
  case error
    Sqlite.OutsideFolder(path: path): "outside #{path}"
    Sqlite.NotPermitted(reason: reason): "denied #{reason}"
    Sqlite.SqlFailed(message: message): "failed #{message}"
  end
end

fn describe_value(value: Sqlite.Value) : Text
  case value
    Sqlite.Null: "null"
    Sqlite.Integer(n): "int #{n}"
    Sqlite.Real(r): "real #{r}"
    Sqlite.String(s): "str #{s}"
    Sqlite.Blob(b): "blob #{b.size}"
  end
end

fn main(console: Console) : Int
  outside = Sqlite.OutsideFolder(path: "../a")
  errors = [outside, Sqlite.NotPermitted(reason: "attach"), Sqlite.SqlFailed(message: "syntax")]
  values = [
    Sqlite.Null,
    Sqlite.Integer(7),
    Sqlite.Real(1.5),
    Sqlite.String("ok"),
    Sqlite.Blob("xy".bytes)
  ]
  rows = Sqlite.Rows(columns: ["c"], rows: [values])
  console.print("#{errors}")
  console.print("#{rows}")
  if describe(outside) == "outside ../a" and
    describe(Sqlite.NotPermitted(reason: "attach")) == "denied attach" and
    describe(Sqlite.SqlFailed(message: "syntax")) == "failed syntax" and
    describe_value(Sqlite.Null) == "null" and
    describe_value(Sqlite.Integer(7)) == "int 7" and
    describe_value(Sqlite.Real(1.5)) == "real 1.5" and
    describe_value(Sqlite.String("ok")) == "str ok" and
    describe_value(Sqlite.Blob("xy".bytes)) == "blob 2" and
    rows.columns == ["c"] and
    rows.rows == [values] and
    "#{errors}" ==
      "[OutsideFolder(path: \"../a\"), NotPermitted(reason: \"attach\"), SqlFailed(message: \"syntax\")]" and
    "#{rows}" ==
      "Rows(columns: [\"c\"], rows: [[Null, Integer(7), Real(1.5), String(\"ok\"), Blob(bytes[120, 121])]])" and
    outside == Sqlite.OutsideFolder(path: "../a") and
    outside != Sqlite.SqlFailed(message: "syntax") and
    outside < Sqlite.SqlFailed(message: "syntax") and
    Sqlite.NotPermitted(reason: "b").compare(Sqlite.OutsideFolder(path: "a")) == Greater and
    outside is Sqlite.OutsideFolder(path: "../a"): 0 else: 1
end

test "sqlite types and ordering"
  rows = Sqlite.Rows(columns: ["a"], rows: [[Sqlite.Integer(1)]])
  assert rows.columns == ["a"]
  assert Sqlite.Integer(1) < Sqlite.String("a")
end
