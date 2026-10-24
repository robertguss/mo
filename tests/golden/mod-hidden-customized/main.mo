module Main
use Shop.Money

fn main(console: Console)
  small = Money.make(150, "EUR")
  large = Money.make(2000, "USD")
  console.print("#{small.name} | #{Money.describe(small)} | #{small.cents}")
  console.print("#{small} #{[small, large]} #{Some(large)}")
  console.print("#{small < large} #{small.compare(large)} #{[small, large].sort}")
  console.print("#{[small, large].max} #{[large, small].min}")
end
