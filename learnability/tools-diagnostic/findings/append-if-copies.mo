## MO-63 finding: appending an `if` expression copies the list every time.
## `mo run` takes about 13 s at N = 40,000; binding the `if` to a name first takes 0.06 s.
module AppendIfCopies

fn main(console: Console)
  var xs = [0].take(0)
  for i in 0..40000
    xs = xs.append(if i % 2 == 0: 0 else: 1)
  end
  console.print("#{xs.size}")
end
