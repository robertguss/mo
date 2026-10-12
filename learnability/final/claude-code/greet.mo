## Greets the name given on the command line, or the world.
module Greet

## Prints one line greeting whoever the arguments name.
fn main(args: Args, console: Console)
  console.print(greeting(args.list))
end

## The greeting for the arguments given on the command line. Together they
## are the name, with one space between its words, so `greet Ada Lovelace`
## greets Ada Lovelace. With no name, or only blank ones, the greeting is to
## the world.
fn greeting(arguments: List(Text)) : Text
  promise "the greeting is one line that says hello, and greets the world when no name is given"
    result.starts_with?("Hello, ")
    result.ends_with?("!")
    not result.contains?("\n")
    arguments.all?(fn(argument) argument.trim.empty? end) implies result == "Hello, world!"
    allows arguments: [], result: "Hello, world!"
    allows arguments: ["Ada"], result: "Hello, Ada!"
    forbids arguments: [], result: "Hello, !"
    forbids arguments: ["Ada\nLovelace"], result: "Hello, Ada\nLovelace!"
  end
  examples
    greeting([]) == "Hello, world!"
    greeting(["Ada"]) == "Hello, Ada!"
    greeting(["Ada", "Lovelace"]) == "Hello, Ada Lovelace!"
    greeting(["  Ada\tLovelace\n"]) == "Hello, Ada Lovelace!"
    greeting(["", " "]) == "Hello, world!"
  end

  name = arguments.join(" ").words.join(" ")
  who = if name.empty?: "world" else: name
  "Hello, #{who}!"
end

test "with no name on the command line, the world is greeted"
  console = Console.fake()
  main(Args.of([]), console)
  assert console.printed == ["Hello, world!"]
  assert console.printed_errors == []
end

test "the name on the command line is greeted, all its words"
  console = Console.fake()
  main(Args.of(["Ada", "Lovelace"]), console)
  assert console.printed == ["Hello, Ada Lovelace!"]
  assert console.printed_errors == []
end
