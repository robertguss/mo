## The core of a bank account: deposits, withdrawals that can't overdraw, and
## transfers between two accounts.
module Account
expose Account, AccountError(..), open, owner, balance, deposit, withdraw, transfer

struct Account
  owner: Text
  balance: Decimal
end

enum AccountError
  InvalidAmount
  InsufficientFunds(short_by: Decimal)
end

## A new account starts empty. Outside this module an Account comes only from
## open, deposit, withdraw and transfer, so its balance is never negative.
fn open(owner: Text) : Account
  Account(owner: owner, balance: 0)
end

fn owner(account: Account) : Text
  account.owner
end

fn balance(account: Account) : Decimal
  account.balance
end

fn deposit(account: Account, amount: Decimal) : Result(Account, AccountError)
  promise "a deposit adds exactly the amount"
    result is Ok(after) implies after.balance == account.balance + amount
    allows account: alice(10.00), amount: 5.50, result: Ok(alice(15.50))
    forbids account: alice(10.00), amount: 5.50, result: Ok(alice(10.00))
  end

  examples
    deposit(alice(10.00), 5.50) == Ok(alice(15.50))
    deposit(alice(10.00), 0) == Error(InvalidAmount)
    deposit(alice(10.00), -1.00) == Error(InvalidAmount)
  end

  return Error(InvalidAmount) if amount <= 0
  var after = account
  after.balance += amount
  Ok(after)
end

fn withdraw(account: Account, amount: Decimal) : Result(Account, AccountError)
  promise "a withdrawal never leaves a negative balance"
    result is Ok(after) implies after.balance >= 0
    allows account: alice(100.00), amount: 30.00, result: Ok(alice(70.00))
    forbids account: alice(50.00), amount: 80.00, result: Ok(alice(-30.00))
  end

  promise "a withdrawal takes exactly the amount asked for"
    result is Ok(after) implies after.balance == account.balance - amount
    allows account: alice(100.00), amount: 30.00, result: Ok(alice(70.00))
    forbids account: alice(100.00), amount: 30.00, result: Ok(alice(75.00))
  end

  examples
    withdraw(alice(100.00), 30.00) == Ok(alice(70.00))
    withdraw(alice(100.00), 100.00) == Ok(alice(0.00))
    withdraw(alice(100.00), 100.01) == Error(InsufficientFunds(short_by: 0.01))
    withdraw(alice(100.00), 0.00) == Error(InvalidAmount)
  end

  return Error(InvalidAmount) if amount <= 0
  return Error(InsufficientFunds(short_by: amount - account.balance)) if amount > account.balance
  var after = account
  after.balance -= amount
  Ok(after)
end

fn transfer(from: Account, to: Account, amount: Decimal) : Result((Account, Account), AccountError)
  promise "a transfer moves money without making or losing any"
    result is Ok((paid, received)) implies paid.balance + received.balance == from.balance + to.balance
    allows from: alice(100.00), to: bob(5.00), amount: 30.00, result: Ok((alice(70.00), bob(35.00)))
    forbids from: alice(100.00), to: bob(5.00), amount: 30.00, result: Ok((alice(70.00), bob(5.00)))
  end

  examples
    transfer(alice(100.00), bob(0), 100.00) == Ok((alice(0), bob(100.00)))
    transfer(alice(10.00), bob(0), 10.01) == Error(InsufficientFunds(short_by: 0.01))
  end

  paid = try withdraw(from, amount)
  received = try deposit(to, amount)
  Ok((paid, received))
end

fn alice(balance: Decimal) : Account
  Account(owner: "alice", balance: balance)
end

fn bob(balance: Decimal) : Account
  Account(owner: "bob", balance: balance)
end

test "a deposit and a withdrawal of the same amount change nothing"
  assert deposit(alice(40.00), 12.50) is Ok(more)
  assert withdraw(more, 12.50) == Ok(alice(40.00))
end

test "a failed transfer leaves both accounts as they were"
  from = alice(10.00)
  to = bob(0)
  assert transfer(from, to, 20.00) is Error(InsufficientFunds(short_by: short))
  assert short == 10.00
  assert from.balance == 10.00 and to.balance == 0
end
