module List
expose map

fn map(items: List(T), transform: fn(T) : U) : List(U)
  var mapped = []
  for item in items
    mapped = mapped.append(transform(item))
  end
  mapped
end
