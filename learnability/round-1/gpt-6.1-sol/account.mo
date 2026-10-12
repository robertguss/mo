## Immutable bank accounts, with amounts in integer cents and hidden balances.
## Successful operations return replacement values; errors change nothing.
module Account
expose Account, AccountError(..), open, owner, balance, deposit, withdraw, transfer

struct Account
  holder: Text
  cents: BigInt
end

enum AccountError
  InvalidAmount
  InsufficientFunds
end

fn open(name: Text) : Account
  promise "a new account belongs to its owner and starts with no money"
    result.holder == name
    result.cents == 0
    allows name: "Ada", result: Account(holder: "Ada", cents: 0.to_big)
    forbids name: "Ada", result: Account(holder: "Ada", cents: 1.to_big)
  end
  examples
    balance(open("Ada")) == 0.to_big
    owner(open("Ada")) == "Ada"
  end

  Account(holder: name, cents: 0.to_big)
end

fn owner(account: Account) : Text
  account.holder
end

fn balance(account: Account) : BigInt
  account.cents
end

fn deposit(account: Account, amount: BigInt) : Result(Account, AccountError)
  promise "positive deposits add exactly their amount without changing the owner"
    case result
      Ok(updated): amount > 0 and updated.holder == account.holder and updated.cents == account.cents + amount
      Error(InvalidAmount): amount <= 0
      Error(InsufficientFunds): false
    end
    allows account: open("Ada"), amount: 5.to_big, result: Ok(Account(holder: "Ada", cents: 5.to_big))
    forbids account: open("Ada"), amount: 5.to_big, result: Ok(open("Ada"))
  end
  examples
    deposit(open("Ada"), 250.to_big) == Ok(Account(holder: "Ada", cents: 250.to_big))
    deposit(open("Ada"), 0.to_big) == Error(InvalidAmount)
  end

  return Error(InvalidAmount) if amount <= 0
  Ok(Account(holder: account.holder, cents: account.cents + amount))
end

fn withdraw(account: Account, amount: BigInt) : Result(Account, AccountError)
  promise "a withdrawal subtracts exactly its amount and cannot overdraw"
    case result
      Ok(updated): amount > 0 and amount <= account.cents and updated.cents >= 0 and
        updated.cents == account.cents - amount and updated.holder == account.holder
      Error(InvalidAmount): amount <= 0
      Error(InsufficientFunds): amount > 0 and amount > account.cents
    end
    allows account: Account(holder: "Ada", cents: 5.to_big), amount: 5.to_big, result: Ok(open("Ada"))
    forbids account: Account(holder: "Ada", cents: 5.to_big), amount: 6.to_big, result: Ok(Account(holder: "Ada", cents: -1.to_big))
  end
  examples
    withdraw(Account(holder: "Ada", cents: 500.to_big), 125.to_big) == Ok(Account(holder: "Ada", cents: 375.to_big))
    withdraw(open("Ada"), 1.to_big) == Error(InsufficientFunds)
  end

  return Error(InvalidAmount) if amount <= 0
  return Error(InsufficientFunds) if amount > account.cents
  Ok(Account(holder: account.holder, cents: account.cents - amount))
end

fn transfer(source: Account, destination: Account, amount: BigInt) : Result((Account, Account), AccountError)
  promise "a transfer moves exactly the amount and neither makes nor loses money"
    case result
      Ok((debited, credited)): amount > 0 and amount <= source.cents and debited.cents >= 0 and
        debited.holder == source.holder and credited.holder == destination.holder and
        debited.cents == source.cents - amount and credited.cents == destination.cents + amount and
        debited.cents + credited.cents == source.cents + destination.cents
      Error(InvalidAmount): amount <= 0
      Error(InsufficientFunds): amount > 0 and amount > source.cents
    end
    allows source: Account(holder: "A", cents: 10.to_big), destination: open("B"), amount: 4.to_big,
      result: Ok((Account(holder: "A", cents: 6.to_big), Account(holder: "B", cents: 4.to_big)))
    forbids source: Account(holder: "A", cents: 10.to_big), destination: open("B"), amount: 4.to_big,
      result: Ok((Account(holder: "A", cents: 6.to_big), Account(holder: "B", cents: 5.to_big)))
  end
  examples
    transfer(Account(holder: "A", cents: 300.to_big), open("B"), 125.to_big) ==
      Ok((Account(holder: "A", cents: 175.to_big), Account(holder: "B", cents: 125.to_big)))
    transfer(open("A"), open("B"), 1.to_big) == Error(InsufficientFunds)
  end

  debited = try withdraw(source, amount)
  credited = try deposit(destination, amount)
  Ok((debited, credited))
end
