module Main

# Each lambda reads a tuple part before any use has decided its parameter's type.
fn main(console: Console)
  pick = fn(entry) entry.1 end
  console.print(pick((1, "x")))
  var seen = {}
  var round = 0
  while round < 2
    earlier = seen.get("a") or []
    case earlier.find(fn(entry) entry.0 > 0 end)
      Some(p): console.print(p.1)
      None:
        console.print("none")
        seen = seen.set("a", [(0, "skip"), (2, "y")])
    end
    round += 1
  end
  nested = fn(e) e.0.1 end
  console.print(nested(((1, "deep"), true)))
end
