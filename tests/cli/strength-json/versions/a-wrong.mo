module Main

fn title(text: Text) : Text
  case Json.parse(text)
    Ok(value): value.text_at("t") or "missing"
    Error(_): "bad"
  end
end
