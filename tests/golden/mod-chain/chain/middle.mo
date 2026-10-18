module Chain.Middle
expose twice
use Chain.Base

fn twice(n: Int) : Int
  Base.add(n, n)
end
