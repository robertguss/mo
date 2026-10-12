/-
  examples/account.mo in Core.

  Exercises: records, a choice type of failures, early `return`, assigning a
  `var`'s field, `try` (reference 8.3), constants, and promises (12.2): each
  promise body lowers to a Bool function of the parameters and `result`, and
  its `allows` and `forbids` anchors are checked.
-/
import MoCore.Testing

namespace MoCore.Examples.Account

open MoCore DSL Lib

/-- `struct Account  owner: Text  balance: Decimal  end` -/
def Account : DataDecl := record "Account" [("owner", tText), ("balance", tDec)]

/-- `enum AccountError  InvalidAmount  InsufficientFunds(short_by: Decimal)  end` -/
def AccountError : DataDecl :=
  choice "AccountError" [("InvalidAmount", []), ("InsufficientFunds", [("short_by", tDec)])]

def resTy : Ty := tRes Account.ty AccountError.ty
def pairTy : Ty := tTup [Account.ty, Account.ty]

def invalidAmount : Expr := mkC AccountError "InvalidAmount" []
def bal (e : Expr) : Expr := get Account "balance" e

/-- `max_amount = 1_000_000_000.00` -/
def maxAmount : ConstDecl := const "max_amount" tDec (dec "1000000000.00")

/-- `fn open(owner: Text) : Account  Account(owner: owner, balance: 0)  end` -/
def openFn : FunDecl := fn "open" [("owner", tText)] Account.ty <| mk Account [v "owner", dec "0"]

/-- `fn owner(account: Account) : Text  account.owner  end` -/
def ownerFn : FunDecl := fn "owner" [("account", Account.ty)] tText <| get Account "owner" (v "account")

/-- `fn balance(account: Account) : Decimal  account.balance  end` -/
def balanceFn : FunDecl := fn "balance" [("account", Account.ty)] tDec <| bal (v "account")

/-- ```
fn deposit(account: Account, amount: Decimal) : Result(Account, AccountError)
  return Error(InvalidAmount) if amount <= 0 or amount > max_amount
  var after = account
  after.balance += amount
  Ok(after)
end
``` -/
def deposit : FunDecl :=
  fn "deposit" [("account", Account.ty), ("amount", tDec)] resTy <|
    ite (or_ (dle (v "amount") (dec "0")) (dgt (v "amount") (global "max_amount")))
      (err_ Account.ty AccountError.ty invalidAmount)
      (lets [("after", v "account"),
             ("after", setField Account "balance" (v "after") (dadd (bal (v "after")) (v "amount")))] <|
        ok_ Account.ty AccountError.ty (v "after"))

/-- ```
fn withdraw(account: Account, amount: Decimal) : Result(Account, AccountError)
  return Error(InvalidAmount) if amount <= 0 or amount > max_amount
  return Error(InsufficientFunds(short_by: amount - account.balance)) if amount > account.balance
  var after = account
  after.balance -= amount
  Ok(after)
end
``` -/
def withdraw : FunDecl :=
  fn "withdraw" [("account", Account.ty), ("amount", tDec)] resTy <|
    ite (or_ (dle (v "amount") (dec "0")) (dgt (v "amount") (global "max_amount")))
      (err_ Account.ty AccountError.ty invalidAmount) <|
    ite (dgt (v "amount") (bal (v "account")))
      (err_ Account.ty AccountError.ty
        (mkC AccountError "InsufficientFunds" [dsub (v "amount") (bal (v "account"))])) <|
    lets [("after", v "account"),
          ("after", setField Account "balance" (v "after") (dsub (bal (v "after")) (v "amount")))] <|
      ok_ Account.ty AccountError.ty (v "after")

/-- ```
fn transfer(from: Account, to: Account, amount: Decimal) : Result((Account, Account), AccountError)
  paid = try withdraw(from, amount)
  received = try deposit(to, amount)
  Ok((paid, received))
end
``` -/
def transfer : FunDecl :=
  fn "transfer" [("from", Account.ty), ("to", Account.ty), ("amount", tDec)]
    (tRes pairTy AccountError.ty) <|
    tryRes pairTy AccountError.ty (call "withdraw" [v "from", v "amount"]) "paid" <|
    tryRes pairTy AccountError.ty (call "deposit" [v "to", v "amount"]) "received" <|
      ok_ pairTy AccountError.ty (tup [v "paid", v "received"])

/-- `fn alice(balance: Decimal) : Account  Account(owner: "alice", balance: balance)  end` -/
def alice : FunDecl := fn "alice" [("balance", tDec)] Account.ty <| mk Account [str "alice", v "balance"]

/-- `fn bob(balance: Decimal) : Account  Account(owner: "bob", balance: balance)  end` -/
def bob : FunDecl := fn "bob" [("balance", tDec)] Account.ty <| mk Account [str "bob", v "balance"]

/-! ### Promises, returning zero or the first false line's number -/

/-- deposit: "a deposit adds exactly the amount"
`result is Ok(after) implies after.balance == account.balance + amount` -/
def depositAddsAmount : FunDecl :=
  promiseFn "deposit.promise.1" [("account", Account.ty), ("amount", tDec), ("result", resTy)]
    [caseRes (v "result") "after" (eq tDec (bal (v "after")) (dadd (bal (v "account")) (v "amount")))
      "_" tt]

/-- withdraw: "a withdrawal never leaves a negative balance"
`result is Ok(after) implies after.balance >= 0` -/
def withdrawNeverNegative : FunDecl :=
  promiseFn "withdraw.promise.1" [("account", Account.ty), ("amount", tDec), ("result", resTy)]
    [caseRes (v "result") "after" (dge (bal (v "after")) (dec "0")) "_" tt]

/-- withdraw: "a withdrawal takes exactly the amount asked for"
`result is Ok(after) implies after.balance == account.balance - amount` -/
def withdrawTakesAmount : FunDecl :=
  promiseFn "withdraw.promise.2" [("account", Account.ty), ("amount", tDec), ("result", resTy)]
    [caseRes (v "result") "after" (eq tDec (bal (v "after")) (dsub (bal (v "account")) (v "amount")))
      "_" tt]

/-- transfer: "a transfer moves money without making or losing any"
`result is Ok((paid, received)) implies paid.balance + received.balance == from.balance + to.balance` -/
def transferKeepsMoney : FunDecl :=
  promiseFn "transfer.promise.1" [("from", Account.ty), ("to", Account.ty), ("amount", tDec),
      ("result", tRes pairTy AccountError.ty)]
    [caseRes (v "result") "pair"
      (eq tDec (dadd (bal (fst (v "pair"))) (bal (snd (v "pair"))))
        (dadd (bal (v "from")) (bal (v "to"))))
      "_" tt]

def program : Program :=
  wholeProgram "Account" "transfer" [Account, AccountError] [maxAmount]
    [openFn, ownerFn, balanceFn, deposit, withdraw, transfer, alice, bob,
     depositAddsAmount, withdrawNeverNegative, withdrawTakesAmount, transferKeepsMoney]

/-- The checker accepts the program; `decide +kernel` has the kernel run it. -/
theorem checked : wellTyped? program = true := by decide +kernel

/-- So the program is well typed, and `type_safety` applies to it. -/
theorem wellTyped : WellTyped program := checkProgram_sound checked

def aliceE (s : String) : Expr := call "alice" [dec s]
def bobE (s : String) : Expr := call "bob" [dec s]
def okA (e : Expr) : Expr := ok_ Account.ty AccountError.ty e
def errA (e : Expr) : Expr := err_ Account.ty AccountError.ty e
def insufficient (s : String) : Expr := mkC AccountError "InsufficientFunds" [dec s]

/-! ### Examples -/

-- deposit(alice(10.00), 5.50) == Ok(alice(15.50))
#guard passes program (eq resTy (call "deposit" [aliceE "10.00", dec "5.50"]) (okA (aliceE "15.50")))
-- deposit(alice(10.00), 0) == Error(InvalidAmount)
#guard passes program (eq resTy (call "deposit" [aliceE "10.00", dec "0"]) (errA invalidAmount))
-- deposit(alice(10.00), -1.00) == Error(InvalidAmount)
#guard passes program (eq resTy (call "deposit" [aliceE "10.00", dec "-1.00"]) (errA invalidAmount))
-- deposit(alice(10.00), 1_000_000_000.01) == Error(InvalidAmount)
#guard passes program
  (eq resTy (call "deposit" [aliceE "10.00", dec "1000000000.01"]) (errA invalidAmount))
-- withdraw(alice(100.00), 30.00) == Ok(alice(70.00))
#guard passes program (eq resTy (call "withdraw" [aliceE "100.00", dec "30.00"]) (okA (aliceE "70.00")))
-- withdraw(alice(100.00), 100.00) == Ok(alice(0.00))
#guard passes program (eq resTy (call "withdraw" [aliceE "100.00", dec "100.00"]) (okA (aliceE "0.00")))
-- withdraw(alice(100.00), 100.01) == Error(InsufficientFunds(short_by: 0.01))
#guard passes program
  (eq resTy (call "withdraw" [aliceE "100.00", dec "100.01"]) (errA (insufficient "0.01")))
-- withdraw(alice(100.00), 0.00) == Error(InvalidAmount)
#guard passes program (eq resTy (call "withdraw" [aliceE "100.00", dec "0.00"]) (errA invalidAmount))
-- transfer(alice(100.00), bob(0), 100.00) == Ok((alice(0), bob(100.00)))
#guard passes program
  (eq (tRes pairTy AccountError.ty) (call "transfer" [aliceE "100.00", bobE "0", dec "100.00"])
    (ok_ pairTy AccountError.ty (tup [aliceE "0", bobE "100.00"])))
-- transfer(alice(10.00), bob(0), 10.01) == Error(InsufficientFunds(short_by: 0.01))
#guard passes program
  (eq (tRes pairTy AccountError.ty) (call "transfer" [aliceE "10.00", bobE "0", dec "10.01"])
    (err_ pairTy AccountError.ty (insufficient "0.01")))

/-! ### Promises: each body accepts its `allows` anchor and rejects its `forbids` anchor -/

-- allows account: alice(10.00), amount: 5.50, result: Ok(alice(15.50))
#guard passes program (promiseHolds "deposit.promise.1" [aliceE "10.00", dec "5.50", okA (aliceE "15.50")])
-- forbids account: alice(10.00), amount: 5.50, result: Ok(alice(10.00))
#guard passes program
  (not_ (promiseHolds "deposit.promise.1" [aliceE "10.00", dec "5.50", okA (aliceE "10.00")]))
-- allows account: alice(100.00), amount: 30.00, result: Ok(alice(70.00))
#guard passes program (promiseHolds "withdraw.promise.1" [aliceE "100.00", dec "30.00", okA (aliceE "70.00")])
-- forbids account: alice(50.00), amount: 80.00, result: Ok(alice(-30.00))
#guard passes program
  (not_ (promiseHolds "withdraw.promise.1" [aliceE "50.00", dec "80.00", okA (aliceE "-30.00")]))
-- allows account: alice(100.00), amount: 30.00, result: Ok(alice(70.00))
#guard passes program (promiseHolds "withdraw.promise.2" [aliceE "100.00", dec "30.00", okA (aliceE "70.00")])
-- forbids account: alice(100.00), amount: 30.00, result: Ok(alice(75.00))
#guard passes program
  (not_ (promiseHolds "withdraw.promise.2" [aliceE "100.00", dec "30.00", okA (aliceE "75.00")]))
-- allows from: alice(100.00), to: bob(5.00), amount: 30.00, result: Ok((alice(70.00), bob(35.00)))
#guard passes program
  (promiseHolds "transfer.promise.1" [aliceE "100.00", bobE "5.00", dec "30.00",
    ok_ pairTy AccountError.ty (tup [aliceE "70.00", bobE "35.00"])])
-- forbids from: alice(100.00), to: bob(5.00), amount: 30.00, result: Ok((alice(70.00), bob(5.00)))
#guard passes program
  (not_ (promiseHolds "transfer.promise.1" [aliceE "100.00", bobE "5.00", dec "30.00",
    ok_ pairTy AccountError.ty (tup [aliceE "70.00", bobE "5.00"])]))

/-- The promises hold of the functions on some inputs, as `mo test` checks them
on generated ones: each promise body, given the parameters and the actual
result. -/
def promisesHoldOn (account : Expr) (other : Expr) (amount : String) : Expr :=
  and_ (promiseHolds "deposit.promise.1" [account, dec amount, call "deposit" [account, dec amount]]) <|
  and_ (promiseHolds "withdraw.promise.1" [account, dec amount, call "withdraw" [account, dec amount]]) <|
  and_ (promiseHolds "withdraw.promise.2" [account, dec amount, call "withdraw" [account, dec amount]]) <|
    promiseHolds "transfer.promise.1" [account, other, dec amount, call "transfer" [account, other, dec amount]]

#guard passes program (promisesHoldOn (aliceE "100.00") (bobE "5.00") "30.00")
#guard passes program (promisesHoldOn (aliceE "100.00") (bobE "5.00") "100.01")
#guard passes program (promisesHoldOn (aliceE "0") (bobE "0") "0.000000000000000001")
#guard passes program (promisesHoldOn (aliceE "1000000000.00") (bobE "7") "1000000000.00")
#guard passes program (promisesHoldOn (aliceE "12.34") (bobE "1") "-5")

/-! ### Tests -/

/- test "a deposit and a withdrawal of the same amount change nothing"
```
assert deposit(alice(40.00), 12.50) is Ok(more)
assert withdraw(more, 12.50) == Ok(alice(40.00))
``` -/
#guard passes program
  (caseRes (call "deposit" [aliceE "40.00", dec "12.50"])
    "more" (eq resTy (call "withdraw" [v "more", dec "12.50"]) (okA (aliceE "40.00")))
    "_" ff)

/- test "money moved back and forth adds up"
```
assert deposit(open("ana"), 50.00) is Ok(ana)
assert transfer(ana, open("ben"), 20.00) is Ok((ana_after, ben))
assert transfer(ben, ana_after, 5.00) is Ok((ben_after, ana_final))
assert ana_final.balance == 35.00 and ben_after.balance == 15.00
``` -/
#guard passes program
  (caseRes (call "deposit" [call "open" [str "ana"], dec "50.00"])
    "ana"
      (caseRes (call "transfer" [v "ana", call "open" [str "ben"], dec "20.00"])
        "p1"
          (lets [("ana_after", fst (v "p1")), ("ben", snd (v "p1"))] <|
            caseRes (call "transfer" [v "ben", v "ana_after", dec "5.00"])
              "p2"
                (lets [("ben_after", fst (v "p2")), ("ana_final", snd (v "p2"))] <|
                  and_ (eq tDec (bal (v "ana_final")) (dec "35.00"))
                    (eq tDec (bal (v "ben_after")) (dec "15.00")))
              "_" ff)
        "_" ff)
    "_" ff)

/-! ### Never wrong -/

/-- `transfer` never goes wrong, for any two accounts and any amount. -/
theorem transfer_never_wrong {ρ : Env}
    (hρ : EnvTy program ρ [("from", Account.ty), ("to", Account.ty), ("amount", tDec)])
    (n : Nat) (w : World) (msg : String) :
    evalProgram n program ρ w (call "transfer" [v "from", v "to", v "amount"]) ≠ .wrong msg :=
  never_wrong (τ := tRes pairTy AccountError.ty) checked (by decide +kernel) hρ n w msg

end MoCore.Examples.Account
