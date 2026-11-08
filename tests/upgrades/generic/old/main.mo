module Main
struct Box(T)
  value: T
end
enum Node
  Leaf(value: Int)
  Branch(children: List(Node))
end
struct State
  box: Box(Node)
end
service
  state State = State(box: Box(value: Leaf(value: 7)))
  tools read
  token from env "TOKEN"
end
fn read(state: State, _now: Time) : Int
  case state.box.value
    Leaf(value: n): n
    Branch(children: nodes): nodes.size
  end
end
