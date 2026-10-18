module Main

fn main(console: Console)
  var total = 0
  for i in 0..10
    continue if i % 2 == 0
    total += i
  end
  console.print("odd sum #{total}")
  var halvings = 0
  var remaining = 100
  while remaining > 1
    remaining = remaining / 2
    halvings += 1
  end
  console.print("halvings #{halvings}")
  for _ in 5..5
    console.print("never")
  end
  var last = 0
  for i in 1..100
    break if i * i > 50
    last = i
  end
  console.print("last #{last}")
  console.print("#{(2..5).to_list} #{(2..5).contains?(5)} #{(2..5).contains?(4)}")
end
