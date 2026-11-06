module Main
struct Ledger
  total: Int
  shares: Int
end
service
  state Ledger = Ledger(total: 1200, shares: 4)
  tools share
  token from env "OPERATOR_TOKEN"
end
fn share(state: Ledger, _now: Time, left: Int) : Int
  state.total / (state.shares - left)
end
