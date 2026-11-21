module Main

struct CounterState
  total: Int
end

actor Counter
  state CounterState = CounterState(total: 10)
  calls add, current
end

fn add(state: CounterState, _now: Time, amount: Int) : (CounterState, Int)
  next = state.total + amount
  (CounterState(total: next), next)
end

fn current(state: CounterState, _now: Time) : Int
  state.total
end

fn bump_twice(counter: Counter, n: Int) : Int
  _first = counter.add(n)
  counter.add(n)
end

fn main(io: Console, counter: Counter)
  total = bump_twice(counter, 6)
  io.print("total=#{total},current=#{counter.current}")
end
