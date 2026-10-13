module Main
enum Reply
Hit(Int)
Miss
end
fn run(reply: Reply, values: List(Int)) : Int
case (reply, values)
(Hit(n), []): n
(Hit(n), [first, .._]): n + first
(Miss, _): 0
end
end
