module Main

fn main(console: Console)
  quiet = Console.fake()
  quiet.print("hidden")
  console.print("shown")
end
