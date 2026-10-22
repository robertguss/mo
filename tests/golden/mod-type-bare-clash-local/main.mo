module Main
use Secret

struct Secret
  name: Text
end

fn show(s: Secret) : Text
  s.name
end

fn main(console: Console)
  _x = Secret.make()
  console.print(show(Secret(name: "local")))
end
