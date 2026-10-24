module Main

fn name(n: Int) : Text
  label = case n
    1 => "one"
    _: "other"
  end
  label
end
