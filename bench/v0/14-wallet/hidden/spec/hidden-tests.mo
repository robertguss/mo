## The hidden tests of task 14-wallet.
module HiddenTests
use Wallet{Wallet, WalletError}

fn funded(amount: Decimal) : Wallet
  case Wallet.deposit(Wallet.open("ana"), amount)
    Ok(wallet): wallet
    Error(_): stop "the tests fund wallets with valid amounts"
  end
end

test "any whole balance can be withdrawn"
  for amount in [0.01, 0.99, 10, 12345.67]
    assert Wallet.withdraw(funded(amount), amount) is Ok(wallet)
    assert Wallet.balance(wallet) == 0
  end
end

test "not a cent more than the balance can be withdrawn"
  assert Wallet.withdraw(funded(10), 10.01) == Error(Insufficient(balance: 10, wanted: 10.01))
  assert Wallet.withdraw(Wallet.open("ana"), 0.01) == Error(Insufficient(balance: 0, wanted: 0.01))
end

test "the whole balance can be transferred"
  assert Wallet.transfer(funded(10), funded(1), 10) is Ok((from, to))
  assert Wallet.balance(from) == 0
  assert Wallet.balance(to) == 11
end

test "a balance built from several deposits can be emptied"
  assert Wallet.deposit(funded(0.10), 0.20) is Ok(wallet)
  assert Wallet.withdraw(wallet, 0.30) is Ok(empty)
  assert Wallet.balance(empty) == 0
end
