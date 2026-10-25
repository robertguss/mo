module Main

fn f(n: Int) : Duration
  one = 0x00_01.minutes
  two = 2.minute
  _convert = Int.minute
  _chain = 1.minutes.in_seconds
  _other = (1).minutes
  _long = 18446744073709551617.second
  _comment = 1. # dot
    milliseconds # field
  one + two + n.minute
end
