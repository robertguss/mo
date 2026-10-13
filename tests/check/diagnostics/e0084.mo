module Main
struct Tree(T)
children: List(Tree(List(T)))
end
