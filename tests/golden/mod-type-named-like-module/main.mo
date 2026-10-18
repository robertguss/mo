module Main
use Account

fn main(console: Console)
  a = Account.open("Ada", 10)
  console.print(Account.describe(a))
  console.print(Account.describe(Account.open("Bo", 0)))
end
