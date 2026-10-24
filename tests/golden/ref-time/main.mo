module Main

fn main(console: Console)
  now = Time.parse("2026-10-12t04:15:00.5+02:00") or stop "valid reference time"
  console.print(now.to_text)
  console.print("#{now + 30.minutes} #{now - 1.hours}")
  console.print("#{(now + 1.days) - now}")
  console.print("#{(200.milliseconds * 3).in_milliseconds} #{(-1500).milliseconds.in_seconds}")
  console.print("#{(-1500).milliseconds.in_milliseconds} #{1.seconds > 200.milliseconds}")
  first = Time.parse("0001-01-01T00:00:00Z") or stop "first instant"
  last = Time.parse("9999-12-31T23:59:59.999999z") or stop "last instant"
  console.print("#{first.checked_add((-1).seconds)} #{last.checked_add(1.seconds)}")
  console.print("#{first < now and now < last}")
  console.print("#{Time.parse("2026-10-12T02:15:00.1234567Z")}")
  console.print("#{Time.parse("2026-10-12T02:15:60Z")}")
  console.print("#{Time.parse("2026-10-12T02:15:00Z")}")
end
