module Main
enum Reply
Yes(value: Int)
No
end
fn run(reply: Reply) : Int
case reply
Yes(n): n
No: 0
end
end
