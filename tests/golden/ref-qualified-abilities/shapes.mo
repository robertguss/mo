module Shapes
expose Shape(..), Money, money

enum Shape
  Circle(radius: Int)
  Square(side: Int)
end

struct Money
  cents: Int
end

fn money(cents: Int) : Money
  Money(cents: cents)
end

fn to_text(amount: Money) : Text
  "$#{amount.cents}"
end

# Dearer first, so the written order differs from the automatic one.
fn compare(a: Money, b: Money) : Ordering
  b.cents.compare(a.cents)
end
