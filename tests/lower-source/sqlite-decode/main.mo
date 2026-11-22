module Main
use Date
use Json

enum Status
  Draft
  Active(day: Int)
end

struct Meta
  tags: List(Text)
  status: Status
end

struct Row
  id: Int
  paid: Bool
  ratio: Float
  title: Text
  raw: Bytes
  amount: Decimal
  created_at: Time
  due_date: Date
  note: Option(Text)
  meta: Meta
end

fn main() : Int
  meta_json = Json.encode(Meta(tags: ["x", "y"], status: Active(day: 5)))
  rows = Sqlite.Rows(
    columns: [
      "note",
      "meta",
      "due_date",
      "created_at",
      "amount",
      "raw",
      "title",
      "ratio",
      "paid",
      "id"
    ],
    rows: [
      [
        Sqlite.Null,
        Sqlite.String(meta_json),
        Sqlite.String("2026-05-01"),
        Sqlite.String("2026-04-19T12:00:00Z"),
        Sqlite.String("99.50"),
        Sqlite.Blob("ok".bytes),
        Sqlite.String("Rent"),
        Sqlite.Integer(2),
        Sqlite.Integer(1),
        Sqlite.Integer(7)
      ]
    ]
  )
  case Sqlite.decode_one[Row](rows)
    Ok(r):
      empty = Sqlite.Rows(columns: rows.columns, rows: [])
      missing = Sqlite.Rows(columns: ["id"], rows: [[Sqlite.Integer(1)]])
      if r.id == 7 and
        r.paid and
        r.ratio == 2.0 and
        r.title == "Rent" and
        r.raw == "ok".bytes and
        r.amount == 99.50 and
        r.note == None and
        r.meta.status == Active(day: 5) and
        Sqlite.decode_optional[Row](empty) == Ok(None) and
        Sqlite.decode[Row](missing) == Error(MissingColumn(name: "paid"))
        0
      else
        1
      end
    Error(_): 2
  end
end
