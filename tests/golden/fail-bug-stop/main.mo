module Main

fn main(console: Console)
  console.print("before")
  rates = {"usd": 1}
  rate = case rates.get("eur")
    Some(r): r
    None: stop "every currency was checked at startup"
  end
  console.print("#{rate}")
end
