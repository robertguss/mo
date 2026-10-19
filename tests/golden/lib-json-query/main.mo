module Main
use Json

fn total(items: List(Json.Json)) : Decimal
  var sum = 0.0
  for item in items
    if item is Json.Object(fields)
      if fields.get("price") is Some(Json.Number(n))
        sum += (n.to_decimal or 0.0)
      end
    end
  end
  sum
end

fn main(console: Console)
  var lines = []
  while console.read_line is Some(line)
    lines = lines.append(line)
  end
  case Json.parse(lines.join("\n"))
    Ok(Json.Array(items)):
      console.print("#{items.size} items, total #{total(items)}")
      console.print(Json.Array(items.take(1)).to_text)
      console.print(
        Json.Object({"count": Json.int(items.size), "big": Json.decimal(total(items))}).pretty
      )
    Ok(other): console.print("not a list: #{other}")
    Error(problem): console.print(
      "line #{problem.line}, column #{problem.column}: #{problem.problem}"
    )
  end
end
