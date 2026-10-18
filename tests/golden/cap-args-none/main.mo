module Main

fn main(args: Args, console: Console) : Int
  case args.list.first
    Some(first): 
      console.print("first is #{first}")
      0
    None:
      console.error("usage: main WORD")
      1
  end
end
