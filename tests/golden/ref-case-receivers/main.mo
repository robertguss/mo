module Main

enum Signal
  Stop
  Go(speed: Int)
end

label = Less.to_text

fn main(console: Console)
  console.print(label)
  console.print("#{Equal.to_text} #{Greater.to_text.char_count}")
  console.print("#{Less.compare(Greater)} #{Greater.compare(Less)} #{Equal.compare(Equal)}")
  console.print("#{Unit.to_text} #{Unit.compare(Unit)}")
  console.print("#{Folder.to_text} #{Other.compare(File)} #{File.compare(Link)}")
  console.print("#{Stop.to_text} #{Stop.compare(Go(speed: 1))}")
end
