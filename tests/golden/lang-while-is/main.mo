module Main

fn main(console: Console)
  var queue = [Some(1), Some(2), None, Some(3)]
  var total = 0
  var round = 0
  while queue.first is Some(head)
    round += 1
    if head is Some(n)
      total += n
    end
    queue = queue.drop(1)
  end
  console.print("#{round} #{total} #{queue.size}")
end
