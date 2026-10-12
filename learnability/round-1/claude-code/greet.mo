## Greets the name given on the command line, or the world.
module Greet

fn main(args: Args, console: Console)
  console.print(greeting(args.list))
end

## The greeting for a name given in words, or for the world when it is blank.
fn greeting(words: List(Text)) : Text
  examples
    greeting(["Ada"]) == "Hello, Ada!"
    greeting(["Ada", "Lovelace"]) == "Hello, Ada Lovelace!"
    greeting([]) == "Hello, world!"
    greeting([" "]) == "Hello, world!"
  end

  name = words.join(" ").trim
  who = if name.empty?: "world" else: name
  "Hello, #{who}!"
end

test "greets the name given on the command line"
  console = Console.fake()
  main(Args.of(["Ada", "Lovelace"]), console)
  assert console.printed == ["Hello, Ada Lovelace!"]
end

test "greets the world when no name is given"
  console = Console.fake()
  main(Args.of([]), console)
  assert console.printed == ["Hello, world!"]
end
