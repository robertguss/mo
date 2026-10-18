module Main

enum QuantityError
  Empty
  NotANumber(text: Text)
end

enum OrderError
  BadQuantity(QuantityError)
  UnknownItem(name: Text)
end

fn parse_quantity(text: Text) : Result(Int, QuantityError)
  trimmed = text.trim
  return Error(Empty) if trimmed.empty?
  case trimmed.to_int
    Some(n): Ok(n)
    None: Error(NotANumber(text: trimmed))
  end
end

fn parse_line(line: Text, prices: Map(Text, Int)) : Result(Int, OrderError)
  parts = line.split(",")
  name = try parts.get(0).or_fail(UnknownItem(name: line))
  quantity = try parse_quantity(parts.get(1) or "")
  price = try prices.get(name).or_fail(UnknownItem(name: name))
  Ok(price * quantity)
end

fn report(line: Text, prices: Map(Text, Int)) : Text
  case parse_line(line, prices)
    Ok(total): "total #{total}"
    Error(BadQuantity(Empty)): "quantity missing"
    Error(BadQuantity(NotANumber(text: t))): "bad quantity #{t}"
    Error(UnknownItem(name: n)): "unknown item #{n}"
  end
end

fn main(console: Console)
  prices = {"pen": 3, "ink": 12}
  for line in ["pen,4", "ink, 2", "pen", "pen,many", "cup,1"]
    console.print(report(line, prices))
  end
end
