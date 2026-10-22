module Box
expose Box(..), of

struct Box(T)
  item: T
end

fn of(item: T) : Box(T)
  Box(item: item)
end
