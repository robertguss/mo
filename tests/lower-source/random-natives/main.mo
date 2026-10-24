module Main

fn main(random: Random) : Int
  if random.int(-3, -2) == -3: 0 else: 1
end

test "seeded streams are independent and aliases share their stream"
  seed = Random.seeded
  draw = Random.int
  first = seed(-1)
  second = Random.seeded(-1)
  alias = first
  for _ in 0..20
    expected = second.int(-1000, 37)
    assert draw(alias, -1000, 37) == expected
    assert first.int(17, 901) == draw(second, 17, 901)
  end
  assert first.int(-3, -2) == -3
  vector = seed(-1)
  assert draw(vector, -1000, 37) == -420
  assert vector.int(-1000, 37) == -205
  assert draw(vector, -1000, 37) == -474
end
