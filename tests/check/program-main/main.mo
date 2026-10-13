module Main

fn main() : Int
  _message = NotFound(path: "notes").to_text
  [3, 4].map(fn(value) value + 2 end).sum +
    List.map([1, 2], fn(value) value + 1 end).sum
end
