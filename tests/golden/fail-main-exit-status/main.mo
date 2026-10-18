module Main

fn grade(score: Int) : Int
  if score >= 90: 0 else if score >= 50: 3 else: 7
end

fn main(args: Args, console: Console) : Int
  score = (args.list.first or "0").to_int or 0
  console.print("score #{score}")
  grade(score)
end
