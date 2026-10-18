module Left
expose value

fn secret() : Int
  1
end

fn value() : Int
  secret() + 10
end
