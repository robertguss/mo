module Main

fn index_of(items: List(Text), wanted: Text) : Option(Int)
  examples
    index_of(["a", "b"], "b") == Some(1)
    index_of(["a", "b"], "z") == None
  end

  var found = None
  for (i, item) in items.enumerate
    if found is None and item == wanted
      found = Some(i)
    end
  end
  found
end

fn main(console: Console)
  names = ["ada", "bo", "cy", "bo"]
  console.print("#{index_of(names, "bo")}")
  console.print("#{index_of(names, "zed")}")
  console.print("#{index_of(names, "cy") or -1}")
end
