module Main

handler = Env.get
fn main(env: Env) : Int
(handler(env, "MO_TEST") or "").byte_count
end
