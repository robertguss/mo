module Main

fn main(console: Console)
  console.print("#{1 < 2 and 2 < 3} #{1 > 2 or 3 > 2} #{not (1 == 1)}")
  console.print("#{1.5 < 2.25} #{2.50 == 2.5} #{-0.1 < 0.0}")
  console.print("#{(1, 2) < (1, 3)} #{(2, 0) < (1, 9)} #{(1, "a") == (1, "a")}")
  console.print("#{[1, 2] < [1, 2, 0]} #{[3] > [2, 9]} #{[1, 2] == [1, 2]}")
  console.print("#{"apple" < "apricot"} #{"Z" < "a"} #{"" < "a"}")
  console.print("#{false < true} #{'a' < 'b'} #{'é' > 'z'}")
  console.print("#{true implies false} #{false implies false}")
end
