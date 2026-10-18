module Main
use Traffic{Light, next}

fn main(console: Console)
  var light = Red
  for _ in 0..4
    console.print("#{light}")
    light = next(light)
  end
end
