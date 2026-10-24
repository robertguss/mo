## Prints each word given on the command line, in capitals.
module Shout

fn main(args: Args, console: Console)
  for word in args.list
    console.print(word.ascii_uppercase)
  end
end
