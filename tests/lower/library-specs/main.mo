module Main
use Shapes

fn main() : Int
  Shapes.label(21).byte_count
end

test "main's own"
  assert main() == 3
end
