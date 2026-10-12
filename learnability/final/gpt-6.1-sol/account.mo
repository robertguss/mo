## Immutable bank accounts. All amounts are whole cents in one currency.
## Callers keep the returned account, or both accounts returned by transfer.
module Account
expose Account, AccountError(..), open, balance, deposit, withdraw, transfer

struct Account
  cents: Int
end

enum AccountError
  InvalidAmount
  InsufficientFunds
  BalanceLimit
end

## Opens an account with no money in it.
fn open() : Account
  promise "a new account has a zero balance"
    result.cents == 0
    allows result: Account(cents: 0)
    forbids result: Account(cents: 1)
  end
  examples
    open() == Account(cents: 0)
  end

  Account(cents: 0)
end

## Reads the balance in cents without exposing the account's representation.
fn balance(account: Account) : Int
  promise "the reported balance is the account's nonnegative balance"
    result == account.cents
    result >= 0
    allows account: Account(cents: 125), result: 125
    forbids account: Account(cents: 125), result: 124
  end
  examples
    balance(open()) == 0
    balance(Account(cents: 125)) == 125
  end

  account.cents
end

## Returns an account with the deposit added, or a declared failure.
fn deposit(account: Account, amount: Int) : Result(Account, AccountError)
  promise "a nonnegative deposit adds exactly its amount unless the balance cannot fit"
    case result
      Ok(updated):
        amount >= 0 and updated.cents >= 0 and
          updated.cents.to_big == account.cents.to_big + amount.to_big
      Error(InvalidAmount): amount < 0
      Error(InsufficientFunds): false
      Error(BalanceLimit): amount >= 0 and account.cents.checked_add(amount) == None
    end
    allows account: Account(cents: 125), amount: 75, result: Ok(Account(cents: 200))
    allows account: Account(cents: 125), amount: -1, result: Error(InvalidAmount)
    forbids account: Account(cents: 125), amount: 75, result: Ok(Account(cents: 201))
    forbids account: Account(cents: 125), amount: 75, result: Error(BalanceLimit)
  end
  examples
    deposit(open(), 125) == Ok(Account(cents: 125))
    deposit(Account(cents: 125), 75) == Ok(Account(cents: 200))
    deposit(Account(cents: 125), 0) == Ok(Account(cents: 125))
    deposit(open(), -1) == Error(InvalidAmount)
    deposit(Account(cents: 9_223_372_036_854_775_807), 1) == Error(BalanceLimit)
  end

  return Error(InvalidAmount) if amount < 0
  next_balance = try account.cents.checked_add(amount).or_fail(BalanceLimit)
  Ok(Account(cents: next_balance))
end

## A zero withdrawal succeeds; a withdrawal greater than the balance fails.
fn withdraw(account: Account, amount: Int) : Result(Account, AccountError)
  promise "a withdrawal subtracts exactly its nonnegative amount and never overdraws"
    case result
      Ok(updated):
        amount >= 0 and amount <= account.cents and updated.cents >= 0 and
          updated.cents.to_big == account.cents.to_big - amount.to_big
      Error(InvalidAmount): amount < 0
      Error(InsufficientFunds): amount >= 0 and amount > account.cents
      Error(BalanceLimit): false
    end
    allows account: Account(cents: 125), amount: 25, result: Ok(Account(cents: 100))
    allows account: Account(cents: 125), amount: 126, result: Error(InsufficientFunds)
    forbids account: Account(cents: 125), amount: 25, result: Ok(Account(cents: 99))
    forbids account: Account(cents: 125), amount: 125, result: Error(InsufficientFunds)
  end
  examples
    withdraw(Account(cents: 125), 25) == Ok(Account(cents: 100))
    withdraw(Account(cents: 125), 125) == Ok(Account(cents: 0))
    withdraw(Account(cents: 125), 126) == Error(InsufficientFunds)
    withdraw(open(), 0) == Ok(Account(cents: 0))
    withdraw(open(), 1) == Error(InsufficientFunds)
    withdraw(Account(cents: 125), -1) == Error(InvalidAmount)
  end

  return Error(InvalidAmount) if amount < 0
  return Error(InsufficientFunds) if amount > account.cents
  Ok(Account(cents: account.cents - amount))
end

## Returns (source, destination), together, only if both operations succeed.
## Inputs are account values, not identities or references to mutable accounts.
## On failure neither input changes and no partial update is returned.
fn transfer(source: Account, destination: Account, amount: Int) : Result((Account, Account), AccountError)
  promise "a transfer debits and credits exactly the same nonnegative amount, or fails"
    case result
      Ok(updated):
        amount >= 0 and amount <= source.cents and
          updated.0.cents >= 0 and updated.1.cents >= 0 and
          updated.0.cents.to_big == source.cents.to_big - amount.to_big and
          updated.1.cents.to_big == destination.cents.to_big + amount.to_big
      Error(InvalidAmount): amount < 0
      Error(InsufficientFunds): amount >= 0 and amount > source.cents
      Error(BalanceLimit):
        amount >= 0 and amount <= source.cents and
          destination.cents.checked_add(amount) == None
    end
    allows source: Account(cents: 125), destination: Account(cents: 40),
      amount: 25, result: Ok((Account(cents: 100), Account(cents: 65)))
    allows source: Account(cents: 125), destination: Account(cents: 40),
      amount: 126, result: Error(InsufficientFunds)
    forbids source: Account(cents: 125), destination: Account(cents: 40),
      amount: 25, result: Ok((Account(cents: 125), Account(cents: 40)))
    forbids source: Account(cents: 125), destination: Account(cents: 40),
      amount: 25, result: Error(InsufficientFunds)
  end
  promise "a successful transfer neither makes nor loses money"
    result is Ok(updated) implies
      updated.0.cents.to_big + updated.1.cents.to_big ==
        source.cents.to_big + destination.cents.to_big
    allows source: Account(cents: 125), destination: Account(cents: 40),
      amount: 25, result: Ok((Account(cents: 100), Account(cents: 65)))
    forbids source: Account(cents: 125), destination: Account(cents: 40),
      amount: 25, result: Ok((Account(cents: 100), Account(cents: 66)))
    forbids source: Account(cents: 125), destination: Account(cents: 40),
      amount: 25, result: Ok((Account(cents: 100), Account(cents: 64)))
  end
  examples
    transfer(Account(cents: 125), Account(cents: 40), 25) ==
      Ok((Account(cents: 100), Account(cents: 65)))
    transfer(Account(cents: 125), Account(cents: 40), 125) ==
      Ok((Account(cents: 0), Account(cents: 165)))
    transfer(Account(cents: 125), Account(cents: 40), 0) ==
      Ok((Account(cents: 125), Account(cents: 40)))
    transfer(Account(cents: 50), Account(cents: 50), 10) ==
      Ok((Account(cents: 40), Account(cents: 60)))
    transfer(Account(cents: 125), Account(cents: 40), 126) == Error(InsufficientFunds)
    transfer(Account(cents: 125), Account(cents: 40), -1) == Error(InvalidAmount)
    transfer(Account(cents: 1), Account(cents: 9_223_372_036_854_775_807), 1) ==
      Error(BalanceLimit)
    transfer(Account(cents: 9_223_372_036_854_775_807), Account(cents: 1), 1) ==
      Ok((Account(cents: 9_223_372_036_854_775_806), Account(cents: 2)))
  end

  debited = try withdraw(source, amount)
  credited = try deposit(destination, amount)
  Ok((debited, credited))
end

test "a caller keeps both transfer results and failures leave the old values intact"
  assert deposit(open(), 170) is Ok(funded)
  assert withdraw(funded, 20) is Ok(source)
  assert deposit(open(), 35) is Ok(destination)
  assert transfer(source, destination, 70) is Ok(moved)
  assert balance(moved.0) == 80
  assert balance(moved.1) == 105
  assert balance(moved.0).to_big + balance(moved.1).to_big ==
    balance(source).to_big + balance(destination).to_big
  assert balance(source) == 150
  assert balance(destination) == 35
  assert transfer(moved.0, moved.1, 81) == Error(InsufficientFunds)
  assert balance(moved.0) == 80
  assert balance(moved.1) == 105
  assert deposit(open(), 9_223_372_036_854_775_807) is Ok(full)
  assert transfer(moved.0, full, 1) == Error(BalanceLimit)
  assert balance(moved.0) == 80
  assert balance(full) == 9_223_372_036_854_775_807
end
