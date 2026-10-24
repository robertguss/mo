module Main

fn lookup(env: Env, key: Text) : Option(Text)
  env.get(key)
end

fn main()
end

test "environment snapshots and native aliases"
  create = Env.of
  get = Env.get
  original = {"K": "left", "EMPTY": ""}
  env = create(original)
  other = Env.of(original.set("K", "right"))
  assert lookup(env, "K") == Some("left")
  assert get(env, "EMPTY") == Some("")
  assert Env.get(other, "K") == Some("right")
  assert lookup(env, "missing") is None
  assert lookup(Env.of({}), "K") is None
end
