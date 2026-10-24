module Main

fn describe(count: Int) : Text
  "#{count} #{case count
    1: "item"
    _: "items"
  end}"
end

fn main(console: Console)
  console.print("#{case 1
    1: "one"
    _: "other"
  end}")
  console.print(describe(1))
  console.print(describe(3))
end
