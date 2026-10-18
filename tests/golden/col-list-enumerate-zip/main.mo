module Main

fn main(console: Console)
  names = ["ada", "grace", "alan"]
  for (i, name) in names.enumerate
    console.print("#{i}: #{name}")
  end
  pairs = names.zip([1, 2])
  console.print("#{pairs.size}")
  for (name, n) in pairs
    console.print("#{name}=#{n}")
  end
  console.print(names.join(", "))
  console.print(names.join(""))
end
