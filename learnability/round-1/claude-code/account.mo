## The core of a bank account: open one, then deposit, withdraw and transfer
## money. Amounts are whole cents, and only an amount above zero moves.
## An account's insides are hidden, so this module's functions are the only way
## to make or change one, and none of them lets a balance go below zero: a
## withdrawal can't overdraw. Accounts are values, so each operation returns a
## new account, and a refused one leaves the caller holding the account it had.
module Account
expose Account, AccountError(..), open, balance, owner, deposit, withdraw, transfer

## A bank account: who holds it, and how much money is in it. Only this module
## sees its fields.
struct Account
  owner: Text
  balance: Int  # in cents, and never below zero
end

## Why an account refused to move money.
enum AccountError
  InvalidAmount                                      # the amount was zero or less
  InsufficientFunds(available: Int, requested: Int)  # the balance can't cover the amount
  BalanceTooLarge                                    # the new balance wouldn't fit in an Int
end

## Opens an account for `owner`. A new account holds nothing.
fn open(owner: Text) : Account
  promise "a new account belongs to its owner and holds nothing"
    result.owner == owner
    result.balance == 0
    allows owner: "Ada", result: Account(owner: "Ada", balance: 0)
    forbids owner: "Ada", result: Account(owner: "Bob", balance: 0)
    forbids owner: "Ada", result: Account(owner: "Ada", balance: 100)
  end
  examples
    open("Ada") == Account(owner: "Ada", balance: 0)
  end

  Account(owner: owner, balance: 0)
end

## The money in the account, in cents.
fn balance(account: Account) : Int
  examples
    balance(open("Ada")) == 0
    balance(Account(owner: "Ada", balance: 1250)) == 1250
  end

  account.balance
end

## The name of the account's holder.
fn owner(account: Account) : Text
  examples
    owner(open("Ada")) == "Ada"
  end

  account.owner
end

## Puts `amount` cents into the account. The amount must be above zero, and the
## new balance must fit in an `Int`.
fn deposit(account: Account, amount: Int) : Result(Account, AccountError)
  promise "a deposit adds exactly the amount, and changes nothing else"
    result is Ok(added) implies
      added == Account(owner: account.owner, balance: account.balance + amount)
    allows account: Account(owner: "Ada", balance: 100), amount: 50,
      result: Ok(Account(owner: "Ada", balance: 150))
    forbids account: Account(owner: "Ada", balance: 100), amount: 50,
      result: Ok(Account(owner: "Ada", balance: 100))
    forbids account: Account(owner: "Ada", balance: 100), amount: 50,
      result: Ok(Account(owner: "Bob", balance: 150))
  end
  promise "only an amount above zero is deposited"
    result is Ok(_) implies amount > 0
    allows account: Account(owner: "Ada", balance: 100), amount: 0,
      result: Error(InvalidAmount)
    forbids account: Account(owner: "Ada", balance: 100), amount: -50,
      result: Ok(Account(owner: "Ada", balance: 50))
  end
  examples
    deposit(Account(owner: "Ada", balance: 100), 50) == Ok(Account(owner: "Ada", balance: 150))
    deposit(Account(owner: "Ada", balance: 100), 0) == Error(InvalidAmount)
    deposit(Account(owner: "Ada", balance: 100), -50) == Error(InvalidAmount)
    deposit(Account(owner: "Ada", balance: 1), 9_223_372_036_854_775_807) == Error(BalanceTooLarge)
  end

  return Error(InvalidAmount) if amount <= 0
  new_balance = try account.balance.checked_add(amount).or_fail(BalanceTooLarge)
  Ok(Account(owner: account.owner, balance: new_balance))
end

## Takes `amount` cents out of the account. The amount must be above zero and
## no more than the balance: a withdrawal can't overdraw.
fn withdraw(account: Account, amount: Int) : Result(Account, AccountError)
  promise "a withdrawal never leaves the balance below zero"
    result is Ok(after) implies after.balance >= 0
    allows account: Account(owner: "Ada", balance: 100), amount: 100,
      result: Ok(Account(owner: "Ada", balance: 0))
    forbids account: Account(owner: "Ada", balance: 100), amount: 150,
      result: Ok(Account(owner: "Ada", balance: -50))
  end
  promise "a withdrawal takes exactly the amount, and changes nothing else"
    result is Ok(taken) implies
      taken == Account(owner: account.owner, balance: account.balance - amount)
    allows account: Account(owner: "Ada", balance: 100), amount: 30,
      result: Ok(Account(owner: "Ada", balance: 70))
    forbids account: Account(owner: "Ada", balance: 100), amount: 30,
      result: Ok(Account(owner: "Ada", balance: 100))
  end
  promise "a withdrawal the balance covers is never refused"
    amount > 0 and amount <= account.balance implies result is Ok(_)
    allows account: Account(owner: "Ada", balance: 100), amount: 100,
      result: Ok(Account(owner: "Ada", balance: 0))
    forbids account: Account(owner: "Ada", balance: 100), amount: 100,
      result: Error(InsufficientFunds(available: 100, requested: 100))
  end
  examples
    withdraw(Account(owner: "Ada", balance: 100), 30) == Ok(Account(owner: "Ada", balance: 70))
    withdraw(Account(owner: "Ada", balance: 100), 100) == Ok(Account(owner: "Ada", balance: 0))
    withdraw(Account(owner: "Ada", balance: 100), 101) ==
      Error(InsufficientFunds(available: 100, requested: 101))
    withdraw(Account(owner: "Ada", balance: 100), 0) == Error(InvalidAmount)
  end

  return Error(InvalidAmount) if amount <= 0
  if amount > account.balance
    Error(InsufficientFunds(available: account.balance, requested: amount))
  else
    Ok(Account(owner: account.owner, balance: account.balance - amount))
  end
end

## Moves `amount` cents from the account `from` to the account `to`, and
## returns both, `from` first. A transfer is all or nothing: when `from` can't
## cover the amount, or `to` can't hold it, it is refused and no money moves.
fn transfer(from: Account, to: Account, amount: Int) : Result((Account, Account), AccountError)
  promise "a transfer neither makes nor loses money"
    result is Ok((from_after, to_after)) implies total(from_after, to_after) == total(from, to)
    allows from: Account(owner: "Ada", balance: 100), to: Account(owner: "Bob", balance: 20),
      amount: 30,
      result: Ok((Account(owner: "Ada", balance: 70), Account(owner: "Bob", balance: 50)))
    forbids from: Account(owner: "Ada", balance: 100), to: Account(owner: "Bob", balance: 20),
      amount: 30,
      result: Ok((Account(owner: "Ada", balance: 70), Account(owner: "Bob", balance: 20)))
    forbids from: Account(owner: "Ada", balance: 100), to: Account(owner: "Bob", balance: 20),
      amount: 30,
      result: Ok((Account(owner: "Ada", balance: 100), Account(owner: "Bob", balance: 50)))
  end
  promise "a transfer takes exactly the amount from the first account and gives it to the second"
    result is Ok((paid, received)) implies
      paid == Account(owner: from.owner, balance: from.balance - amount) and
      received == Account(owner: to.owner, balance: to.balance + amount)
    allows from: Account(owner: "Ada", balance: 100), to: Account(owner: "Bob", balance: 20),
      amount: 30,
      result: Ok((Account(owner: "Ada", balance: 70), Account(owner: "Bob", balance: 50)))
    forbids from: Account(owner: "Ada", balance: 100), to: Account(owner: "Bob", balance: 20),
      amount: 30,
      result: Ok((Account(owner: "Ada", balance: 130), Account(owner: "Bob", balance: -10)))
  end
  promise "a transfer never overdraws the account it takes from"
    result is Ok((drawn, _)) implies drawn.balance >= 0
    allows from: Account(owner: "Ada", balance: 100), to: Account(owner: "Bob", balance: 20),
      amount: 100,
      result: Ok((Account(owner: "Ada", balance: 0), Account(owner: "Bob", balance: 120)))
    forbids from: Account(owner: "Ada", balance: 100), to: Account(owner: "Bob", balance: 20),
      amount: 130,
      result: Ok((Account(owner: "Ada", balance: -30), Account(owner: "Bob", balance: 150)))
  end
  examples
    transfer(Account(owner: "Ada", balance: 100), Account(owner: "Bob", balance: 20), 30) ==
      Ok((Account(owner: "Ada", balance: 70), Account(owner: "Bob", balance: 50)))
    transfer(Account(owner: "Ada", balance: 100), Account(owner: "Bob", balance: 20), 130) ==
      Error(InsufficientFunds(available: 100, requested: 130))
    transfer(Account(owner: "Ada", balance: 100), Account(owner: "Bob", balance: 20), 0) ==
      Error(InvalidAmount)
  end

  debited = try withdraw(from, amount)
  credited = try deposit(to, amount)
  Ok((debited, credited))
end

## An account's text form: its holder and its balance, as in `Ada: 12.50`.
fn to_text(account: Account) : Text
  examples
    to_text(Account(owner: "Ada", balance: 1250)) == "Ada: 12.50"
    to_text(open("Bob")) == "Bob: 0.00"
  end

  "#{account.owner}: #{(account.balance.to_decimal / 100).to_fixed(2)}"
end

## The money two accounts hold together. The sum is a `BigInt`, so it fits
## however large the balances are.
fn total(a: Account, b: Account) : BigInt
  examples
    total(Account(owner: "Ada", balance: 100), Account(owner: "Bob", balance: 20)) == 120.to_big
  end

  a.balance.to_big + b.balance.to_big
end

test "a withdrawal cannot overdraw"
  assert deposit(open("Ada"), 100) is Ok(ada)
  assert withdraw(ada, 101) == Error(InsufficientFunds(available: 100, requested: 101))
  assert withdraw(ada, 100) is Ok(emptied)
  assert emptied.balance == 0
  assert withdraw(emptied, 1) == Error(InsufficientFunds(available: 0, requested: 1))
end

test "a transfer moves money from one account to the other, and none is made or lost"
  assert deposit(open("Ada"), 100) is Ok(ada)
  bob = open("Bob")
  assert transfer(ada, bob, 30) is Ok((ada_after, bob_after))
  assert ada_after.balance == 70
  assert bob_after.balance == 30
  assert total(ada_after, bob_after) == total(ada, bob)
end

test "a transfer is refused when the payer cannot cover it or the payee cannot hold it"
  assert deposit(open("Ada"), 100) is Ok(ada)
  assert deposit(open("Bob"), 9_223_372_036_854_775_807) is Ok(bob)
  assert transfer(ada, bob, 101) == Error(InsufficientFunds(available: 100, requested: 101))
  assert transfer(ada, bob, 1) == Error(BalanceTooLarge)
  assert transfer(bob, ada, 1) is Ok(_)
end

test "only an amount above zero moves"
  ada = open("Ada")
  bob = open("Bob")
  assert deposit(ada, 0) == Error(InvalidAmount)
  assert deposit(ada, -100) == Error(InvalidAmount)
  assert withdraw(ada, -100) == Error(InvalidAmount)
  assert transfer(ada, bob, -100) == Error(InvalidAmount)
end

test "an account prints as its holder and balance"
  assert deposit(open("Ada"), 1250) is Ok(ada)
  assert "#{ada}" == "Ada: 12.50"
end
