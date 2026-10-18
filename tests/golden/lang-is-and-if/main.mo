module Main

enum Reply
  Body(Text)
  Rejected(code: Int)
  Pending
end

fn show(reply: Reply) : Text
  if reply is Body(body) and body.char_count > 3
    "long body #{body}"
  else if reply is Body(body)
    "short body #{body}"
  else if reply is Rejected(code: c) and c >= 500
    "server error #{c}"
  else if reply is Rejected(code: _)
    "client error"
  else
    "waiting"
  end
end

fn main(console: Console)
  replies = [Body("hello"), Body("hi"), Rejected(code: 503), Rejected(code: 404), Pending]
  for r in replies
    console.print(show(r))
  end
  sign = if replies.size < 0: "negative" else if replies.size == 0: "zero" else: "positive"
  console.print(sign)
  maybe = Some(4)
  if maybe is Some(n) and n > 3
    console.print("big #{n}")
  end
end
