module Main
use UpgradeOld
struct State
  result: Int
end
service
  state State = State(result: 0)
  tools read
  token from env "TOKEN"
end
fn read(state: State, _now: Time) : Int
  state.result
end
fn migrate(old: UpgradeOld.State) : State
  result = case old.box.value
    UpgradeOld.Leaf(value: n): n
    UpgradeOld.Branch(children: nodes): nodes.size
  end
  State(result: result)
end
