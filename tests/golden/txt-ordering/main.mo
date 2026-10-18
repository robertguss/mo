module Main

fn main(console: Console)
  words = ["pear", "Apple", "apple", "banana", "", "zebra", "éclair"]
  console.print("#{words.sort_by_desc(fn(s) s end)}")
  console.print("#{["bb", "a", "cc", "d"].sort_by_desc(fn(s) s.char_count end)}")
  console.print("#{['b', 'A', 'a'].sort_by_desc(fn(c) c.code end)}")
  console.print("#{"abc" < "abd"} #{"abc" < "ab"} #{"ab" < "abc"}")
end
