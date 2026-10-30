module Types
expose Reply(..), Box(..)
enum Reply
  Item(Box(Int))
end
struct Box(T)
  value: (T, List(Hidden))
end
struct Hidden
  value: Int
end
