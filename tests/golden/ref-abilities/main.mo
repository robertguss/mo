module Main

fn order(value: Ordering) : Text
  case value
    Less: "less"
    Equal: "equal"
    Greater: "greater"
  end
end

fn whole_sum(items: List(BigInt)) : BigInt
  items.sum
end

fn decimal_sum(items: List(Decimal)) : Decimal
  items.sum
end

fn float_sum(items: List(Float)) : Float
  items.sum
end

fn main(console: Console)
  console.print(order(1.compare(2)))
  console.print("#{Unit == Unit} #{Unit < Unit} #{Unit <= Unit}")
  console.print("#{ {Unit: 4}.get(Unit)} #{ {false: 1, true: 2}.get(true)}")
  a = 1.to_byte or stop "fits"
  b = 255.to_byte or stop "fits"
  console.print("#{a < b} #{a != b} #{ {b: 8}.get(b)}")
  console.print("#{ {1.5: 7}.get(1.50)} #{ {'é': 9}.get('\u{e9}')}")
  big = 9223372036854775807.to_big + 1
  console.print("#{ {big: 6}.get(big)}")
  console.print("#{"A".bytes < "B".bytes} #{"A".bytes == "A".bytes}")
  console.print("#{ {"A".bytes: 3}.get("A".bytes)}")
  console.print("#{ {(1..4): 2}.get(1..4)} #{(1..4) == (1..4)}")
  console.print("#{ {[1, 2]: 5}.get([1, 2])}")
  console.print("#{ { {"a": 1, "b": 2}: 6}.get({"b": 2, "a": 1})}")
  first = Set.empty.add(2).add(1)
  second = Set.empty.add(1).add(2)
  console.print("#{first == second} #{ {first: 7}.get(second)}")
  console.print(first.to_text)
  time = Time.parse("2026-10-12T00:00:00Z") or stop "valid time"
  console.print("#{ {time: 8}.get(time)} #{ {2.seconds: 9}.get(2000.milliseconds)}")
  nan = 0.0.to_float / 0.0
  console.print("#{[nan, 3.0, -1.0, nan, 2.0].sort}")
  console.print("#{whole_sum([2, 3])} #{whole_sum([])}")
  console.print("#{decimal_sum([0.1, 0.2])} #{decimal_sum([])}")
  console.print("#{float_sum([0.5, 0.25])} #{float_sum([])}")
end
