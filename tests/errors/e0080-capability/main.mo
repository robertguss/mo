module Main

fn same?(a: Console, b: Console) : Bool
  a == b
end

fn show(clock: Clock) : Text
  "#{clock}"
end

fn keyed(files: Files) : Set(Files)
  Set.of([files])
end
