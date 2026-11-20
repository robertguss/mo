module Main

fn main(console: Console, network: Network)
  console.print("before")
  console.print("#{network.connect("localhost", 65536, 1.second) is Ok(_)}")
  console.print("unreachable")
end
