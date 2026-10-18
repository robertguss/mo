module Main

fn label(flag: Bool) : Text
  if flag
    text = "yes"
    text
  else
    text = "no"
    text
  end
end

fn pick(n: Int) : Text
  case n
    1:
      note = "one"
      note
    _:
      note = "other"
      note
  end
end

fn main(console: Console)
  trimmed = "  x ".trim
  console.print("[#{trimmed}]")
  console.print("#{label(true)} #{label(false)}")
  console.print("#{pick(1)} #{pick(2)}")
  var total = 0
  for _ in 0..3
    total += 2
  end
  for i in 0..3
    for j in 0..2
      total += i * j
    end
  end
  console.print("#{total}")
  (a, b) = (1, 2)
  console.print("#{a + b}")
end
