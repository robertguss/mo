module Traffic
expose Light(..), shade

enum Light
  Red
  Green
end

fn shade(light: Light) : Text
  case light
    Red: "red"
    Green: "green"
  end
end
