module Main

enum Choice
  First(Text)
  Second(Text)
end

fn show(text: Text, name: Text) : Text
  "#{name}#{text}"
end

fn pick(a: Choice, b: Choice, name: Text) : Text
  x = case a
    First(t): t
    Second(_):
      y = case b
        First(_): return "b"
        Second(_):
          z = case b
            First(_): return "c"
            Second(_): return "d"
          end
          show(z, name)
      end
      show(y, name)
  end
  show(x, name)
end

fn nested(a: Choice, b: Choice, name: Text) : Text
  x = case a
    First(t): t
    Second(_):
      case b
        First(_): return "b"
        Second(_): return "d"
      end
  end
  show(x, name)
end

fn partial(a: Choice, name: Text) : Text
  x = case a
    First(t): return t
    Second(t): t
  end
  show(x, name)
end

fn main() : Int
  assert pick(First("x"), Second("y"), "n") == "nx"
  assert pick(Second("x"), First("y"), "n") == "b"
  assert pick(Second("x"), Second("y"), "n") == "d"
  assert nested(First("x"), Second("y"), "n") == "nx"
  assert nested(Second("x"), First("y"), "n") == "b"
  assert nested(Second("x"), Second("y"), "n") == "d"
  assert partial(First("x"), "n") == "x"
  assert partial(Second("x"), "n") == "nx"
  0
end
