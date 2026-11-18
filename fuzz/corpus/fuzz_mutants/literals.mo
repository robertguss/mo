module Main

fn pick(n: Int, flag: Bool) : Int
  return 0x9F if not flag or false
  return 1 if n is 4
  case n
    7: 9_999 + n * 2
    _: 0b1 - n / 3
  end
end
