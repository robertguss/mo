## A boundary fixture for export sizing: its state grows by whole KiB of text
## until saving refuses the next state.
module Main

service
  state List(Text) = []
  tools grow, size
  token from env "GROW_TOKEN"
end

## Appends one text of `kib` KiB and returns the number of texts held.
fn grow(state: List(Text), _now: Time, kib: Int) : (List(Text), Int)
  next = state.append("x".repeat(kib * 1024))
  (next, next.size)
end

## The number of texts held and their total bytes.
fn size(state: List(Text), _now: Time) : (Int, Int)
  var bytes = 0
  for text in state
    bytes += text.byte_count
  end
  (state.size, bytes)
end
