## The core of a bank account: open one, deposit into it, withdraw from it,
## and transfer money between two of them. Money is counted in whole cents,
## as an Int.
##
## An account's insides are hidden: other modules can't build one, take one
## apart or read its fields, only pass it to the functions here. So every
## account has come from open, deposit, withdraw and transfer, and none of
## them lets a balance go below zero.
module Account
expose Account, AccountError(..), open, owner, balance, deposit, withdraw, transfer

## A bank account: whose it is, and how many cents it holds, which is never
## fewer than zero.
struct Account
  owner: Text
  balance: Int
end

## Why a deposit, a withdrawal or a transfer was refused.
enum AccountError
  InvalidAmount(amount: Int)                    # the amount isn't above zero
  InsufficientFunds(balance: Int, amount: Int)  # the balance doesn't cover the amount
  BalanceTooLarge                               # the new balance wouldn't fit in an Int
end

## Opens an account for an owner, with nothing in it.
fn open(owner: Text) : Account
  promise "a new account belongs to its owner and holds no money"
    result == Account(owner: owner, balance: 0)
    allows owner: "Ada", result: Account(owner: "Ada", balance: 0)
    forbids owner: "Ada", result: Account(owner: "Ada", balance: 100)
    forbids owner: "Ada", result: Account(owner: "Bo", balance: 0)
  end
  examples
    open("Ada") == Account(owner: "Ada", balance: 0)
    balance(open("Ada")) == 0
  end

  Account(owner: owner, balance: 0)
end

## Whose account it is.
fn owner(account: Account) : Text
  examples
    owner(open("Ada")) == "Ada"
  end

  account.owner
end

## How many cents the account holds.
fn balance(account: Account) : Int
  # mo test builds accounts only through the functions this module exposes, as
  # other modules must, so this checks that no series of calls can overdraw one.
  promise "no account is ever overdrawn"
    result >= 0
    allows account: Account(owner: "Ada", balance: 500), result: 500
    forbids account: Account(owner: "Ada", balance: -100), result: -100
  end
  examples
    balance(open("Ada")) == 0
    balance(Account(owner: "Ada", balance: 500)) == 500
  end

  account.balance
end

## Puts money into an account. The amount must be above zero, and the new
## balance must fit in an Int.
fn deposit(account: Account, amount: Int) : Result(Account, AccountError)
  promise "a deposit adds exactly the amount, and keeps the owner"
    result is Ok(after) implies
      after.balance == account.balance + amount and after.owner == account.owner
    allows account: Account(owner: "Ada", balance: 500), amount: 200,
      result: Ok(Account(owner: "Ada", balance: 700))
    forbids account: Account(owner: "Ada", balance: 500), amount: 200,
      result: Ok(Account(owner: "Ada", balance: 600))
    forbids account: Account(owner: "Ada", balance: 500), amount: 200,
      result: Ok(Account(owner: "Bo", balance: 700))
  end
  promise "a deposit is refused only when its amount isn't above zero, or the new balance wouldn't fit"
    amount <= 0 implies result == Error(InvalidAmount(amount: amount))
    amount > 0 and account.balance.checked_add(amount) is Some(_) implies result is Ok(_)
    allows account: Account(owner: "Ada", balance: 500), amount: -5,
      result: Error(InvalidAmount(amount: -5))
    allows account: Account(owner: "Ada", balance: 9_223_372_036_854_775_807), amount: 1,
      result: Error(BalanceTooLarge)
    forbids account: Account(owner: "Ada", balance: 500), amount: -5,
      result: Ok(Account(owner: "Ada", balance: 495))
    forbids account: Account(owner: "Ada", balance: 500), amount: 200,
      result: Error(BalanceTooLarge)
  end
  examples
    deposit(open("Ada"), 500) == Ok(Account(owner: "Ada", balance: 500))
    deposit(Account(owner: "Ada", balance: 500), 1) == Ok(Account(owner: "Ada", balance: 501))
    deposit(open("Ada"), 0) == Error(InvalidAmount(amount: 0))
    deposit(open("Ada"), -5) == Error(InvalidAmount(amount: -5))
    deposit(Account(owner: "Ada", balance: 9_223_372_036_854_775_807), 1) == Error(BalanceTooLarge)
  end

  return Error(InvalidAmount(amount: amount)) if amount <= 0
  var updated = account
  updated.balance = try account.balance.checked_add(amount).or_fail(BalanceTooLarge)
  Ok(updated)
end

## Takes money out of an account. The amount must be above zero and no more
## than the balance, so a withdrawal never overdraws.
fn withdraw(account: Account, amount: Int) : Result(Account, AccountError)
  promise "a withdrawal never overdraws: it takes exactly the amount, only from a balance that covers it, and keeps the owner"
    result is Ok(after) implies
      after.balance == account.balance - amount and after.balance >= 0 and
      after.owner == account.owner
    amount > account.balance implies
      result == Error(InsufficientFunds(balance: account.balance, amount: amount))
    allows account: Account(owner: "Ada", balance: 500), amount: 500,
      result: Ok(Account(owner: "Ada", balance: 0))
    allows account: Account(owner: "Ada", balance: 500), amount: 501,
      result: Error(InsufficientFunds(balance: 500, amount: 501))
    forbids account: Account(owner: "Ada", balance: 500), amount: 501,
      result: Ok(Account(owner: "Ada", balance: -1))
    forbids account: Account(owner: "Ada", balance: 500), amount: 200,
      result: Ok(Account(owner: "Ada", balance: 400))
  end
  promise "a withdrawal is refused only when its amount isn't above zero, or the balance doesn't cover it"
    amount <= 0 implies result == Error(InvalidAmount(amount: amount))
    amount > 0 and amount <= account.balance implies result is Ok(_)
    allows account: Account(owner: "Ada", balance: 500), amount: 0,
      result: Error(InvalidAmount(amount: 0))
    forbids account: Account(owner: "Ada", balance: 500), amount: 0,
      result: Ok(Account(owner: "Ada", balance: 500))
    forbids account: Account(owner: "Ada", balance: 500), amount: 200,
      result: Error(InsufficientFunds(balance: 500, amount: 200))
  end
  examples
    withdraw(Account(owner: "Ada", balance: 500), 200) == Ok(Account(owner: "Ada", balance: 300))
    withdraw(Account(owner: "Ada", balance: 500), 500) == Ok(Account(owner: "Ada", balance: 0))
    withdraw(Account(owner: "Ada", balance: 500), 501) ==
      Error(InsufficientFunds(balance: 500, amount: 501))
    withdraw(open("Ada"), 1) == Error(InsufficientFunds(balance: 0, amount: 1))
    withdraw(open("Ada"), 0) == Error(InvalidAmount(amount: 0))
  end

  return Error(InvalidAmount(amount: amount)) if amount <= 0
  if amount > account.balance
    return Error(InsufficientFunds(balance: account.balance, amount: amount))
  end
  var updated = account
  updated.balance -= amount
  Ok(updated)
end

## Moves money from one account to another: a withdrawal from `from` and a
## deposit into `to`, which happen together or not at all. A refused transfer
## changes nothing, since the caller still holds both accounts as they were.
fn transfer(from: Account, to: Account, amount: Int) : Result((Account, Account), AccountError)
  promise "a transfer neither makes nor loses money"
    # The sums are BigInts, so that adding two large balances can't overflow.
    result is Ok((from_after, to_after)) implies
      from_after.balance.to_big + to_after.balance.to_big ==
        from.balance.to_big + to.balance.to_big
    allows from: Account(owner: "Ada", balance: 500), to: Account(owner: "Bo", balance: 100),
      amount: 200,
      result: Ok((Account(owner: "Ada", balance: 300), Account(owner: "Bo", balance: 300)))
    forbids from: Account(owner: "Ada", balance: 500), to: Account(owner: "Bo", balance: 100),
      amount: 200,
      result: Ok((Account(owner: "Ada", balance: 300), Account(owner: "Bo", balance: 100)))
    forbids from: Account(owner: "Ada", balance: 500), to: Account(owner: "Bo", balance: 100),
      amount: 200,
      result: Ok((Account(owner: "Ada", balance: 500), Account(owner: "Bo", balance: 300)))
  end
  promise "a transfer moves exactly the amount, and never overdraws the account it comes from"
    result is Ok((from_after, to_after)) implies
      from_after.balance == from.balance - amount and from_after.balance >= 0 and
      to_after.balance == to.balance + amount
    amount > from.balance implies
      result == Error(InsufficientFunds(balance: from.balance, amount: amount))
    allows from: Account(owner: "Ada", balance: 500), to: Account(owner: "Bo", balance: 100),
      amount: 500,
      result: Ok((Account(owner: "Ada", balance: 0), Account(owner: "Bo", balance: 600)))
    allows from: Account(owner: "Ada", balance: 500), to: Account(owner: "Bo", balance: 100),
      amount: 501,
      result: Error(InsufficientFunds(balance: 500, amount: 501))
    forbids from: Account(owner: "Ada", balance: 500), to: Account(owner: "Bo", balance: 100),
      amount: 501,
      result: Ok((Account(owner: "Ada", balance: -1), Account(owner: "Bo", balance: 601)))
  end
  examples
    transfer(Account(owner: "Ada", balance: 500), Account(owner: "Bo", balance: 100), 200) ==
      Ok((Account(owner: "Ada", balance: 300), Account(owner: "Bo", balance: 300)))
    transfer(Account(owner: "Ada", balance: 500), Account(owner: "Bo", balance: 100), 501) ==
      Error(InsufficientFunds(balance: 500, amount: 501))
    transfer(open("Ada"), open("Bo"), 0) == Error(InvalidAmount(amount: 0))
    transfer(Account(owner: "Ada", balance: 500),
      Account(owner: "Bo", balance: 9_223_372_036_854_775_807), 1) == Error(BalanceTooLarge)
  end

  debited = try withdraw(from, amount)
  credited = try deposit(to, amount)
  Ok((debited, credited))
end

## An account's text form shows its owner and its balance in units and cents,
## as in "Ada: 12.50", rather than its hidden insides.
fn to_text(account: Account) : Text
  examples
    to_text(Account(owner: "Ada", balance: 1250)) == "Ada: 12.50"
    to_text(Account(owner: "Ada", balance: 5)) == "Ada: 0.05"
    to_text(open("Bo")) == "Bo: 0.00"
  end

  "#{account.owner}: #{(account.balance.to_decimal / 100).to_fixed(2)}"
end

test "money moved between two accounts is all still there"
  assert deposit(open("Ada"), 1_000) is Ok(ada)
  assert deposit(open("Bo"), 250) is Ok(bo)
  assert transfer(ada, bo, 400) is Ok((ada_after, bo_after))
  assert balance(ada_after) == 600
  assert balance(bo_after) == 650
  assert balance(ada_after) + balance(bo_after) == balance(ada) + balance(bo)
end

test "the whole balance can be taken out, but not a cent more"
  assert deposit(open("Ada"), 500) is Ok(ada)
  assert withdraw(ada, 501) == Error(InsufficientFunds(balance: 500, amount: 501))
  assert withdraw(ada, 500) is Ok(emptied)
  assert balance(emptied) == 0
  assert withdraw(emptied, 1) == Error(InsufficientFunds(balance: 0, amount: 1))
  assert transfer(emptied, open("Bo"), 1) == Error(InsufficientFunds(balance: 0, amount: 1))
end
