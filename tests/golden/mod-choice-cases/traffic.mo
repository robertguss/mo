module Traffic
expose Light(..), next

enum Light
  Red
  Green
  Yellow
end

fn next(light: Light) : Light
  case light
    Red: Green
    Green: Yellow
    Yellow: Red
  end
end
