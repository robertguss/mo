module Main

struct Version
  major: Int
  minor: Int
end

enum Level
  Low
  Mid
  High
end

fn name(o: Ordering) : Text
  case o
    Less: "Less"
    Equal: "Equal"
    Greater: "Greater"
  end
end

fn main(console: Console)
  a = Version(major: 1, minor: 9)
  b = Version(major: 2, minor: 0)
  console.print("#{a < b} #{a >= b} #{a == b} #{a != b}")
  console.print("#{Low < High} #{High <= Mid}")
  console.print("#{name(a.compare(b))} #{name(b.compare(a))} #{name(a.compare(a))}")
  console.print("#{[1, 2] < [1, 2, 0]} #{(1, 5) < (2, 0)} #{"apple" < "banana"}")
  console.print("#{false < true} #{None < Some(0)} #{Ok(9) < Error(0)}")
end
