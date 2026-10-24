## A wallet of money that can't go below zero.
module Wallet
expose Wallet, WalletError(..), open, balance, deposit, withdraw, transfer

struct Wallet
  owner: Text
  balance: Decimal
end

enum WalletError
  NotPositive(amount: Decimal)
  TooManyPlaces(amount: Decimal)
  Insufficient(balance: Decimal, wanted: Decimal)
end

fn open(owner: Text) : Wallet
  Wallet(owner: owner, balance: 0)
end

fn balance(wallet: Wallet) : Decimal
  wallet.balance
end

## An amount is more than zero, in whole cents.
fn check(amount: Decimal) : Result(Decimal, WalletError)
  examples
    check(0.5) == Ok(0.5)
    check(0) == Error(NotPositive(amount: 0))
    check(0.001) == Error(TooManyPlaces(amount: 0.001))
  end

  return Error(NotPositive(amount: amount)) if amount <= 0
  return Error(TooManyPlaces(amount: amount)) if amount % 0.01 != 0
  Ok(amount)
end

fn deposit(wallet: Wallet, amount: Decimal) : Result(Wallet, WalletError)
  checked = try check(amount)
  Ok(Wallet(owner: wallet.owner, balance: wallet.balance + checked))
end

fn withdraw(wallet: Wallet, amount: Decimal) : Result(Wallet, WalletError)
  promise "a withdrawal never leaves the balance below zero"
    case result
      Ok(taken): taken.balance >= 0
      Error(_): true
    end
    allows wallet: Wallet(owner: "ana", balance: 10), amount: 10, result: Ok(
      Wallet(owner: "ana", balance: 0)
    )
    forbids wallet: Wallet(owner: "ana", balance: 10), amount: 11, result: Ok(
      Wallet(owner: "ana", balance: -1)
    )
  end

  checked = try check(amount)
  return Error(Insufficient(balance: wallet.balance, wanted: checked)) if checked > wallet.balance
  Ok(Wallet(owner: wallet.owner, balance: wallet.balance - checked))
end

## Moves `amount` from one wallet to the other, or changes neither.
fn transfer(from: Wallet, to: Wallet, amount: Decimal) : Result((Wallet, Wallet), WalletError)
  taken = try withdraw(from, amount)
  given = try deposit(to, amount)
  Ok((taken, given))
end
