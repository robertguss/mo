module Main

to_int = Byte.to_int
initial = to_int(255.to_byte or stop "byte")

fn main(console: Console) : Int
  zero = 0.to_byte or stop "byte"
  low = 127.to_byte or stop "byte"
  high = 128.to_byte or stop "byte"
  last = 255.to_byte or stop "byte"
  convert = Byte.to_int
  console.print("#{zero.to_int}/#{Byte.to_int(low)}/#{convert(high)}/#{to_int(last)}/#{initial}")
  if zero.to_int == 0 and Byte.to_int(low) == 127 and convert(high) == 128 and
    to_int(last) == 255 and initial == 255: 0 else: 1
end
