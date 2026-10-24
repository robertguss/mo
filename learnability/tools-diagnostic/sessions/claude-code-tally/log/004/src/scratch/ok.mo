module Ok

fn f(x: Int) : Int
  examples
    f(1) == 3
  end

  x + 1
end

test "t"
  assert f(1) == 2
end
