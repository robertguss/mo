module Main

struct Money
  cents: Int
  currency: Text
end

fn to_text(money: Money) : Text
  "#{(money.cents.to_decimal / 100).to_fixed(2)} #{money.currency}"
end

fn main(console: Console)
  price = Money(cents: 1250, currency: "EUR")
  console.print("#{price}")
  console.print(price.to_text)
  console.print("#{[price, Money(cents: 5, currency: "USD")]}")
end
