module Shop.Account
expose Account, open, describe

struct Account
  owner: Text
  balance: Int
end

fn open(owner: Text, balance: Int) : Account
  Account(owner: owner, balance: balance)
end

fn describe(account: Account) : Text
  "#{account.owner} has #{account.balance}"
end
