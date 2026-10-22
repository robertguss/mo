module Secret
expose Secret, make

struct Secret
  value: Int
end

fn make() : Secret
  Secret(value: 1)
end
