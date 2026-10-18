module Main

fn main(console: Console)
  t = "the quick brown fox"
  console.print("#{t.starts_with?("the")} #{t.starts_with?("quick")} #{t.ends_with?("fox")} #{t.ends_with?("the")}")
  console.print("#{t.contains?("quick")} #{t.contains?("slow")} #{t.contains?("n f")}")
  console.print("#{t.drop_prefix("the ")} #{t.drop_prefix("a")}")
  console.print("#{t.drop_suffix(" fox")} #{t.drop_suffix("cat")}")
  console.print("#{t.drop_prefix(t)}")
end
