module Main

fn greet_all(console: Console, names: List(Text))
  for name in names
    console.print("hello #{name}")
  end
  if names.empty?
    console.error("nobody to greet")
  end
end

fn echo_first(console: Console) : Text
  console.read_line or "(none)"
end

test "a fake collects both streams"
  console = Console.fake()
  greet_all(console, ["Ada", "Grace"])
  greet_all(console, [])
  assert console.printed == ["hello Ada", "hello Grace"]
  assert console.printed_errors == ["nobody to greet"]
end

test "a fake can be handed input"
  console = Console.fake_input(["first", "second"])
  assert echo_first(console) == "first"
  assert echo_first(console) == "second"
  assert echo_first(console) == "(none)"
end
