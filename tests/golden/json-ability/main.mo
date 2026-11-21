module Main
use Json

struct User
  name: Text
  role: Role
  bio: Option(Text)
end

enum Role
  Admin
  TeamLead
  Guest(Int)
end

struct Money
  cents: Int
end

fn to_json(m: Money) : Json
  Json.int(m.cents)
end

fn from_json(v: Json) : Result(Money, Text)
  case v
    Json.Number(n):
      cents = try n.to_int.or_fail("expected whole cents")
      return Error("negative cents") if cents < 0
      Ok(Money(cents: cents))
    Json.Null | Json.Boolean(_) | Json.String(_) | Json.Array(_) | Json.Object(_): Error(
      "expected integer cents"
    )
  end
end

fn round_trip(value: T) : Result(T, Json.JsonDecodeError) where T: Json
  Json.decode_strict[T](Json.encode(value))
end

fn main(console: Console)
  u = User(name: "Ada", role: TeamLead, bio: None)
  console.print(Json.encode(u))
  case Json.decode[User]("{\"name\":\"Ada\",\"role\":\"team_lead\",\"extra\":true}")
    Ok(decoded): console.print("#{decoded == u}")
    Error(e): console.print("err: #{e}")
  end
  case Json.decode_strict[User]("{\"name\":\"Ada\",\"role\":\"team_lead\",\"extra\":true}")
    Ok(_): console.print("unexpected ok")
    Error(e): console.print(e.to_text)
  end
  console.print("#{round_trip(Guest(4)) == Ok(Guest(4))}")
  console.print("#{round_trip(Money(cents: 995)) == Ok(Money(cents: 995))}")
  case Json.decode[Money]("-5")
    Ok(_): console.print("unexpected ok")
    Error(e): console.print(e.to_text)
  end
  raw_user = Json.Object({
    "name": Json.String("Ada"),
    "role": Json.String("team_lead"),
    "extra": Json.Boolean(true)
  })
  console.print("#{Json.from_json[User](raw_user) == Ok(u)}")
  case Json.from_json_strict[User](raw_user)
    Ok(_): console.print("unexpected ok")
    Error(e): console.print(e.to_text)
  end
  console.print(
    "#{Json.schema[User]().list_at("required") == Some([Json.String("name"), Json.String("role")])}"
  )
end
