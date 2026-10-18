module Main

fn grow(a: BigInt, b: BigInt) : BigInt
  a * b
end

fn console_free(label: Text, n: BigInt) : Text
  "#{label}=#{n}"
end

fn main(console: Console)
  two64 = 0x10000000000000000
  console.print(console_free("two64", two64))
  console.print(console_free("square", grow(two64, two64)))
  console.print(console_free("sum", two64 + two64))
  console.print(console_free("diff", two64 - grow(two64, two64)))
  console.print(console_free("div", grow(two64, two64) / two64))
  console.print(console_free("rem", grow(two64, two64) % 1000))
  console.print("#{two64 > two64 - 1} #{two64 == two64} #{-two64 < 0}")
end
