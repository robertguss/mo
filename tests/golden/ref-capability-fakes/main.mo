module Main

test "environment snapshots distinguish empty values from absent names"
  make = Env.of
  get = Env.get
  original = {"MODE": "left", "EMPTY": ""}
  env = make(original)
  other = Env.of(original.set("MODE", "right"))
  assert env.get("MODE") == Some("left")
  assert Env.get(other, "MODE") == Some("right")
  assert get(env, "EMPTY") == Some("")
  assert env.get("MISSING") is None
  assert Env.of({}).get("MODE") is None
end

test "seeded random aliases share a stream and equal seeds reproduce it"
  make = Random.seeded
  draw = Random.int
  first = make(-17)
  second = Random.seeded(-17)
  alias = first
  for _ in 0..20
    value = draw(alias, -7, 23)
    assert value >= -7 and value < 23
    assert value == second.int(-7, 23)
    assert first.int(100, 173) == draw(second, 100, 173)
  end
  assert first.int(-3, -2) == -3
end
