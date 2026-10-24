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

fn compare_values(a: T, b: T) : Text where T: Order
  "#{a.compare(b)} #{b.compare(a)} #{a.compare(a)}"
end

fn float_text(value: Float) : Text
  value.to_text
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
  comparison = Int.compare
  console.print("#{Int.compare(-2, 7)} #{comparison(7, -2)} #{comparison(7, 7)}")
  less = Less
  equal = Equal
  greater = Greater
  console.print("#{less} #{equal.to_text} #{greater}")
  console.print(compare_values(less, greater))
  console.print(compare_values(Unit, Unit))
  console.print(compare_values(false, true))
  console.print(compare_values(-2, 7))
  console.print(compare_values(-big, big))
  console.print(compare_values(1.25, 1.5))
  console.print(compare_values(a, b))
  console.print(compare_values(1.to_float, 2.to_float))
  console.print(compare_values("Z", "é"))
  console.print(compare_values('Z', 'é'))
  console.print(compare_values("A".bytes, "B".bytes))
  console.print(compare_values(time, time + 1.seconds))
  console.print(compare_values((-1).seconds, 1.seconds))
  console.print(compare_values((1, 9), (2, 0)))
  console.print(compare_values([1, 2], [1, 2, 0]))
  console.print(compare_values(None, Some(1)))
  console.print(compare_values(Ok(9), Error(0)))
  entry = Entry(name: "a\n\"λ", kind: Folder)
  console.print(entry.to_text)
  console.print(compare_values(entry, Entry(name: "z", kind: File)))
  console.print("#{ {"b": Some(Less), "a": None}}")
  console.print("#{ {"a": [1, 2]}.to_text} #{ {"a": 1}.remove("a").to_text}")
  console.print("#{float_text(0.000001)} #{float_text(0.0000001)}")
  console.print("#{float_text(100000000000000000000.0)} #{float_text(1000000000000000000000.0)}")
end
