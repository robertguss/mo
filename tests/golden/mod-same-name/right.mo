module Right
expose value

fn secret() : Int
  2
end

fn value() : Int
  secret() + 20
end
