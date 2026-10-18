module Money
expose Money, Choice(..), total

struct Money
  amount: Int
end

struct Line
  amount: Int
end

enum Choice
  Ready
  Done
end

secret = 1

fn total(money: Money) : Int
  money.amount
end

fn total(line: Line) : Int
  line.amount
end
