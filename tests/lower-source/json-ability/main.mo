module Main
use Json

struct Profile
  login: Text
  nickname: Option(Text)
end

enum Status
  Ready
  InProgress
  Delayed(Int)
  Fault(code: Int, note: Option(Text))
end

struct Amount
  cents: Int
end

fn to_json(a: Amount) : Json
  Json.int(a.cents)
end

fn from_json(v: Json) : Result(Amount, Text)
  case v
    Json.Number(n):
      c = try n.to_int.or_fail("bad cents")
      return Error("negative") if c < 0
      Ok(Amount(cents: c))
    Json.Null | Json.Boolean(_) | Json.String(_) | Json.Array(_) | Json.Object(_): Error(
      "not number"
    )
  end
end

struct Slug
  raw: Text
end

fn to_json(s: Slug) : Json
  Json.String(s.raw)
end

fn from_json(v: Json) : Result(Slug, Text)
  case v
    Json.String(raw):
      return Error("empty slug") if raw.empty?
      Ok(Slug(raw: raw))
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.Array(_) | Json.Object(_): Error(
      "not string"
    )
  end
end

struct Invoice
  id: Slug
  total: Amount
end

enum Chain
  End
  Cons(head: Int, tail: Chain)
end

fn echo_json(x: T) : Result(T, Json.JsonDecodeError) where T: Json
  Json.decode_strict[T](Json.encode(x))
end

fn main() : Int
  p1 = Profile(login: "ada", nickname: None)
  if Json.decode[Profile]("{\"login\":\"ada\",\"ignored\":1}") != Ok(p1)
    return 1
  end
  if not (Json.decode_strict[Profile]("{\"login\":\"ada\",\"ignored\":1}") is Error(_))
    return 2
  end
  if echo_json(InProgress) != Ok(InProgress)
    return 3
  end
  if echo_json(Delayed(9)) != Ok(Delayed(9))
    return 4
  end
  if Json.decode_strict[Status]("{\"fault\":{\"code\":500}}") != Ok(Fault(code: 500, note: None))
    return 5
  end
  if echo_json(Invoice(id: Slug(raw: "inv-1"), total: Amount(cents: 450))) !=
    Ok(Invoice(id: Slug(raw: "inv-1"), total: Amount(cents: 450)))
    return 6
  end
  if Json.decode[Invoice]("{\"id\":\"inv-1\",\"total\":-1}") !=
    Error(Json.Mismatch(path: "total", message: "negative"))
    return 7
  end
  if Json.decode[Invoice]("{\"id\":\"\",\"total\":10}") !=
    Error(Json.Mismatch(path: "id", message: "empty slug"))
    return 11
  end
  chain = Cons(head: 3, tail: Cons(head: 4, tail: End))
  if echo_json(chain) != Ok(chain)
    return 8
  end
  sch = Json.schema[Profile]()
  if sch.text_at("type") != Some("object")
    return 9
  end
  rec_sch = Json.schema[Chain]()
  if rec_sch.list_at("anyOf").map(fn(xs) xs.size end) != Some(2)
    return 10
  end
  0
end
