module Main

max_attempts = 3
greeting = "hello #{name}"
name = "world"
limit = max_attempts * 10 + offset()
all_names = [name, "again"]

fn offset() : Int
  max_attempts
end

fn main(console: Console)
  console.print(greeting)
  console.print("#{max_attempts} #{limit}")
  console.print("#{all_names}")
end
