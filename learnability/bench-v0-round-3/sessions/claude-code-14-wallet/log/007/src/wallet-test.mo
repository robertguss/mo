## Tests of the Wallet module.
module WalletTest
use Wallet{Wallet, WalletError}

fn funded(amount: Decimal) : Wallet
  case Wallet.deposit(Wallet.open("ana"), amount)
    Ok(wallet): wallet
    Error(_): stop "the tests fund wallets with valid amounts"
  end
end

test "a new wallet is empty"
  assert Wallet.balance(Wallet.open("ana")) == 0
end

test "deposits add to the balance"
  assert Wallet.deposit(funded(10), 2.50) is Ok(wallet)
  assert Wallet.balance(wallet) == 12.5
end

test "amounts are positive and in whole cents"
  assert Wallet.deposit(funded(10), 0) == Error(NotPositive(amount: 0))
  assert Wallet.deposit(funded(10), -5) == Error(NotPositive(amount: -5))
  assert Wallet.withdraw(funded(10), 0.125) == Error(TooManyPlaces(amount: 0.125))
end

test "withdrawals take from the balance"
  assert Wallet.withdraw(funded(10), 4) is Ok(wallet)
  assert Wallet.balance(wallet) == 6
end

test "the whole balance can be withdrawn"
  assert Wallet.withdraw(funded(10), 10) is Ok(wallet)
  assert Wallet.balance(wallet) == 0
end

test "a withdrawal can't overdraw"
  assert Wallet.withdraw(funded(10), 25) == Error(Insufficient(balance: 10, wanted: 25))
end

test "a transfer moves money from one wallet to the other"
  assert Wallet.transfer(funded(10), Wallet.open("bo"), 3) is Ok((from, to))
  assert Wallet.balance(from) == 7
  assert Wallet.balance(to) == 3
end

test "a transfer that can't be made changes nothing"
  assert Wallet.transfer(funded(10), Wallet.open("bo"), 11) ==
    Error(Insufficient(balance: 10, wanted: 11))
end
