# Where lines end and where they continue (reference 2.4).

## A doc comment, then a function whose signature spans lines.
fn transfer(
  from: Account,
  to: Account,
  amount: Decimal,
) : Result((Account, Account), AccountError)
  Ok((from, to))
end

fn biggest(items: List(T)) : Option(T)
  where T: Order
  items.sort.last
end

total = price +
  tax -
  discount
ok = ready
  and paid
  or waived
names = people
  .filter(fn(p) p.active? end)
  .map(fn(p) p.name end)
point = Point(
  x: 1,
  y: 2
)
list = [
  1,
  2,
]
map = {
  "a": 1,
  "b": 2,
}
case shape
  Circle
  | Square: 1
  Triangle |
  Line: 2
end


# Blank lines and comments between statements don't matter.
x = 1   # a comment after code

y = 2
