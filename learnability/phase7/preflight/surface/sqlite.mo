## A stand-in for the proposed Sqlite capability type and its values.
module Sqlite
expose Sqlite, Value(..), Rows(..), SqliteError(..), sqlite, in_memory, run, query, to_text

struct Sqlite
  name: Text
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

fn sqlite(_files: Files, _path: Text) : Result(Sqlite, SqliteError)
  stop "a stand-in"
end

fn in_memory() : Sqlite
  Sqlite(name: "")
end

fn run(_database: Sqlite, _statement: Text, _values: List(Value)) : Result(Unit, SqliteError)
  stop "a stand-in"
end

fn query(_database: Sqlite, _statement: Text, _values: List(Value)) : Result(Rows, SqliteError)
  stop "a stand-in"
end

fn to_text(_value: Value) : Text
  stop "a stand-in"
end
