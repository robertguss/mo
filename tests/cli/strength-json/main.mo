module Main
use Json

fn title(text: Text) : Text
  examples
    title("{\"t\":\"a\"}") == "a"
    title("[]") == "none"
  end

  case Json.parse(text)
    Ok(value): value.text_at("t") or "none"
    Error(_): "bad"
  end
end

fn main(console: Console)
  console.print(title("{\"t\":\"x\"}"))
end
