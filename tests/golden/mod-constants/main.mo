module Main
use Config{limit, label}

doubled = limit * 2
greeting = "#{label}: #{doubled}"

fn main(console: Console)
  console.print(greeting)
  console.print("#{Config.later}")
end
