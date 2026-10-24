module Main

fn main(console: Console)
  console.print("#{if false: 1 else: 2 + 3 * 4}")
  console.print("#{true implies false or true} #{true or false and false}")
  console.print("#{not 1 == 2} #{1 + 2 * 3 == 7}")
  console.print("#{(1 + 1..2 * 3).to_list}")
  console.print("#{20 - 6 - 3} #{24 / 4 / 2} #{-3 * 2}")
  console.print("#{(2, [4, 9]).1.get(1) or 0}")
  console.print("#{Some(3) is Some(n) implies n > 2}")
  negative = case -1
    -1: "negative"
    _: "other"
  end
  decimal = case 2.5
    2.5: "decimal"
    _: "other"
  end
  head = case [2, 3]
    []: 0
    [first, .._]: first
  end
  console.print(negative)
  console.print(decimal)
  console.print("#{head}")
end
