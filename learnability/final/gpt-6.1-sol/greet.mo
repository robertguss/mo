## Greets the first command-line name, or the world.
module Greet

fn main(args: Args, console: Console)
  name = args.list.first or "world"
  console.print("Hello, #{name}!")
end
