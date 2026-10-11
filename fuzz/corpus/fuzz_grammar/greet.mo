## Greets the name given on the command line, or the world.
module Greet

fn greeting(name: Option(Text)) : Text
  examples
    greeting(Some("Ada")) == "Hello, Ada!"
    greeting(None) == "Hello, world!"
  end

  "Hello, #{name or "world"}!"
end

fn main(args: Args, console: Console)
  console.print(greeting(args.list.first))
end

test "only the first argument is used"
  console = Console.fake()
  main(Args.of(["Ada", "Grace"]), console)
  assert console.printed == ["Hello, Ada!"]
end
