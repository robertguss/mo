module Shop.Money
expose Money, cents, describe, make, name

struct Money
  cents: Int
  name: Text
end

fn make(cents: Int, name: Text) : Money
  Money(cents: cents, name: name)
end

fn name(money: Money) : Text
  "money named #{money.name}"
end

fn describe(money: Money) : Text
  money.name
end

fn cents(money: Money) : Int
  money.cents
end

fn to_text(money: Money) : Text
  "#{(money.cents.to_decimal / 100).to_fixed(2)} #{money.name}"
end

fn compare(a: Money, b: Money) : Ordering
  b.cents.compare(a.cents)
end
