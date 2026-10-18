module Main

fn main(args: Args, console: Console) : Int
  console.print("#{args.list.size} arguments")
  for (i, arg) in args.list.enumerate
    console.print("#{i}: #{arg}")
  end
  if args.list.empty?: 1 else: 0
end
