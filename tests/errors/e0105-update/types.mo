module Types
expose Private, Hidden, Visible(..), Reply(..)
struct Private
  value: Hidden
end
struct Hidden
  value: Int
end
struct Visible(T)
  value: T
  next: Option(Visible(T))
end
struct Reply
  value: Option(Visible(Hidden))
end
