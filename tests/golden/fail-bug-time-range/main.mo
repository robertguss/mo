module Main

fn main(console: Console)
  last = Time.parse("9999-12-31T23:59:59Z") or stop "valid time"
  console.print("#{last.checked_add(1.seconds)}")
  console.print("#{last + 999.milliseconds}")
  console.print("#{last + 1.seconds}")
  console.print("unreachable")
end
