module Main

struct Pair
  left: Int
  right: Text
end

fn main(console: Console) : Int
  scale = 3
  label = "n"
  pair = Pair(left: 4, right: "r")
  items = [1, 2]
  mixed = fn(x) "#{label}#{x * scale + pair.left}#{pair.right}#{items.size}" end
  add_scale = fn(x) x + scale end
  nested = fn(x)
    inner = fn(y) add_scale(y) * x end
    inner(x + 1)
  end
  var total = 10
  snapshot = total
  read_later = fn() snapshot + scale end
  total += 5
  console.print("#{mixed(2)}/#{nested(2)}/#{read_later()}/#{total}")
  if mixed(2) == "n10r2" and nested(2) == 12 and read_later() == 13 and total == 15: 0 else: 1
end
