module Main
use Shop.Account
use Bank{Account}

fn number(account: Account) : Int
  account.number
end

fn main(console: Console)
  _a = Account.open("Ada", 10)
  console.print("#{number(Account(number: 7))}")
end
