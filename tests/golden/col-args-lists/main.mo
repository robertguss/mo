module Main

fn main(args: Args, console: Console)
  console.print("#{args.list.size}")
  for (i, arg) in args.list.enumerate
    console.print("#{i} #{arg}")
  end
  console.print(args.list.reverse.join(" "))
end
