module Main

initial = [3, -8, 12].sum

fn total(xs: List(T)) : T where T: Number
  xs.sum
end

fn ints(xs: List(Int)) : Int
  total(xs)
end

fn bigs(xs: List(BigInt)) : BigInt
  total(xs)
end

fn decimals(xs: List(Decimal)) : Decimal
  total(xs)
end

fn floats(xs: List(Float)) : Float
  total(xs)
end

fn main(console: Console) : Int
  add = List.sum
  values = [3, -8, 12]
  console.print("#{initial}/#{values.sum}/#{List.sum([-4, -9, -2])}/#{add([42])}")
  console.print("#{ints([])}/#{bigs([])}/#{decimals([])}/#{floats([])}")
  if initial == 7 and values.sum == 7 and values == [3, -8, 12] and
    List.sum([-4, -9, -2]) == -15 and add([42]) == 42 and
    ints([]) == 0 and bigs([]) == 0x0 and decimals([]) == 0.0 and floats([]) == 0.0 and
    bigs([0x7fffffffffffffff, 0x1, -0x2]) == 0x7ffffffffffffffe and
    decimals([1.25, -3.5, 0.125]) == -2.125 and
    floats([1.5, -4.0, 0.25]) == -2.25 and
    floats([9007199254740992.0, 1.0, -9007199254740992.0]) == 0.0 and
    ints([9223372036854775807, -1]) == 9223372036854775806 and
    ints([-9223372036854775808, 1]) == -9223372036854775807: 0 else: 1
end
