## A stand-in for the proposed Sqlite capability type, the database it opens and its values.
module Sqlite
expose Sqlite, Database, Value(..), Rows(..), SqliteError(..), open, fake, run, query, to_text

struct Sqlite
  name: Text
end

struct Database
  path: Text
end

enum Value
  Null
  Integer(Int)
  Real(Float)
  String(Text)
  Blob(Bytes)
end

struct Rows
  columns: List(Text)
  rows: List(List(Value))
end

enum SqliteError
  OutsideFolder(path: Text)
  NotPermitted(reason: Text)
  SqlFailed(message: Text)
end

fn open(_sqlite: Sqlite, path: Text) : Result(Database, SqliteError)
  Ok(Database(path: path))
end

fn fake() : Sqlite
  Sqlite(name: "")
end

fn run(_database: Database, _statement: Text, _values: List(Value)) : Result(Unit, SqliteError)
  stop "a stand-in"
end

fn query(_database: Database, _statement: Text, _values: List(Value)) : Result(Rows, SqliteError)
  stop "a stand-in"
end

fn to_text(_value: Value) : Text
  stop "a stand-in"
end
