module Main
use Command

fn main(console: Console)
  cmd = Command.fake({})
  if cmd.run("mo", ["check"], 1.second) is Ok(_)
    console.print("ran")
  end
end
