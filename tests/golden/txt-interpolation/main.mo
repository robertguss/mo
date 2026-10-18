module Main

fn main(console: Console)
  name = "Ada"
  count = 3
  console.print("Hello, #{name}! You have #{count} new messages.")
  console.print("sum: #{1 + 2}, nested: #{"x#{name}y"}")
  console.print("list #{[1, 2]} tuple #{(1, true)} opt #{Some(3)}")
  console.print("texts #{["a", "b"]} chars #{['x', 'y']}")
  console.print("write \#{name} to insert")
  console.print("#{"a" == "a"} #{'c'} #{"q"} #{1.50} #{true}")
end
