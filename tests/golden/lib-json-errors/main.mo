module Main
use Json

fn show(text: Text) : Text
  case Json.parse(text)
    Ok(value): "ok #{value.to_text}"
    Error(e): "#{e.line}:#{e.column} #{e.problem}"
  end
end

fn main(console: Console)
  for text in [
    "{\"a\": [1, 2,]}",
    "{\n  \"a\": 1,\n  \"a\": 2\n}",
    "[\"\\ud800\"]",
    "[1.]",
    "\"never closed",
    "[] []",
    "#{"[".repeat(129)}"
  ]
    console.print(show(text))
  end
  console.print("#{Json.float(0.0 / 0.0)} #{Json.float(2.5).map(fn(j) j.to_text end)}")
end
