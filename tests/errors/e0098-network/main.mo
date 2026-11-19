module Main

fn main(console: Console)
  network = Network.fake({})
  if network.connect("example.org", 80, 1.second) is Ok(_)
    console.print("connected")
  end
end
