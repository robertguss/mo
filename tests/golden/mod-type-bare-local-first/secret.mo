module Secret
expose make

struct Secret
  value: Int
end

fn make() : Secret
  Secret(value: 1)
end
