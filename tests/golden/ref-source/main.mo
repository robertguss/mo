## Source spelling and line continuation do not depend on indentation.
module Main

## Every trailing comma here is allowed by Appendix B.
fn add(left: Int, right: Int,) : Int
left + right
end

enum Reply
  Named(value: Int,)
end

fn main(console: Console)
_ignored = "unused on purpose"
ready? = true
snake_case_2 = add(
  7,
  4,
)
items = [
  2,
  3,
]
pair = (snake_case_2, ready?,)
mapping = {"x": 8,}
reply = Named(value: 9,)
sum =
  1 +
  2
  * 3
chain = " é "
  .trim
  .char_count
console.print("#{pair} #{items} #{mapping.get("x")} #{reply}")
console.print("#{sum} #{chain} \u{200B}".char_count.to_text)
for _ in 0..1
console.print("# is text") # é is allowed in comments
end
end
