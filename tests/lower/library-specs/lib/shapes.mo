module Shapes
expose label

fn label(n: Int) : Text
  examples
    label(2) == "#2"
  end
  promise "is never empty"
    result.byte_count > 0
    allows result: "#1", n: 1
    forbids result: "", n: 1
  end

  "##{n}"
end

test "label of 0"
  assert label(0) == "#0"
end
