module Main
use Json

fn read_input(console: Console) : Text
  var lines = []
  while console.read_line is Some(line)
    lines = lines.append(line)
  end
  lines.join("\n")
end

fn main(console: Console)
  case Json.parse(read_input(console))
    Error(problem): console.print("line #{problem.line}, column #{problem.column}")
    Ok(reply):
      console.print("#{reply.text_at("items.0.user.login")}")
      console.print("#{reply.int_at("items.1.number")}")
      console.print("#{reply.int_at("items.0.number")}")
      console.print("#{reply.bool_at("items.1.open")}")
      console.print("#{reply.text_at("items.2.title")}")
      console.print("#{reply.text_at("items.0.user.login.first")}")
      console.print("#{reply.get("a.b")}")
      console.print("#{reply.path("a.b")}")
      for item in (reply.list_at("items") or [])
        console.print("#{item.get("number")} #{item.at(0)}")
      end
      console.print("#{reply.path("0.1").map(fn(found) found.to_text end)}")
      console.print("#{reply.list_at("").map(fn(items) items.size end)}")
  end
end
