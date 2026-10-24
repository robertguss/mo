module Main

batch = limit * 2
limit = positive(0)

fn positive(n: Int) : Int
  if n > 0: n else: stop "limit must be positive"
end

fn main(console: Console)
  console.print("starting")
  console.print("#{batch}")
end
