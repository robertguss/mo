module Main

fn verb(method: Text) : Int
  case method
    "GET": 1
    "POST" | "PUT": 2
    _: 0
  end
end

fn letter(c: Char) : Text
  case c
    'a': "first"
    'z': "last"
    _: "middle"
  end
end

fn main(console: Console)
  console.print("#{verb("GET")} #{verb("PUT")} #{verb("HEAD")}")
  console.print("#{letter('a')} #{letter('m')} #{letter('z')}")
  answer = case 1 < 2
    true: "yes"
    false: "no"
  end
  console.print(answer)
end
