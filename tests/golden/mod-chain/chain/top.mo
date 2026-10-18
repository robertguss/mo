module Chain.Top
expose render
use Chain.Middle

fn render(n: Int) : Text
  "[#{Middle.twice(n)}]"
end
