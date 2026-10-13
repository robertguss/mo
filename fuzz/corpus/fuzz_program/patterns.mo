module Main
fn inspect(value: Result(Option(Int), Text)) : Int
  case value
    Ok(Some(n)) if n < 0: -n
    Ok(Some(n)) | Ok(Some(n)): n
    Ok(None): 0
    Error(_message): -1
  end
end
fn positive(value: Option(Int)) : Int
  if value is Some(n) and n > 0
    n
  else
    0
  end
end
fn main() : Int
  inspect(Ok(Some(4))) + positive(Some(3))
end
