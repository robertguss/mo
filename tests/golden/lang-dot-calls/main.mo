module Main

struct Account
  owner: Text
  balance: Int
end

struct Job
  run: fn() : Int
end

fn deposit(account: Account, amount: Int) : Account
  Account(owner: account.owner, balance: account.balance + amount)
end

fn summary(account: Account) : Text
  "#{account.owner} has #{account.balance}"
end

fn shout(words: List(Text)) : List(Text)
  words.map(fn(w) w.ascii_uppercase end)
end

fn main(console: Console)
  account = Account(owner: "ana", balance: 5)
  console.print(account.deposit(10).deposit(1).summary)
  console.print(summary(deposit(account, 2)))
  words = "a b c".split(" ")
  console.print("#{words.shout}")
  console.print("#{shout(words) == words.shout}")
  console.print(" padded ".trim.ascii_uppercase)
  job = Job(run: fn() 7 end)
  later = fn(j) j.run() end
  console.print("#{job.run()} #{later(job)}")
end
