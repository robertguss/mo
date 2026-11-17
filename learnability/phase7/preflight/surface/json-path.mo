## A stand-in for the lookup functions proposed for the Json library module.
module JsonPath
expose path, text_at, int_at, list_at
use Json

fn path(_value: Json, _steps: Text) : Option(Json)
  stop "a stand-in"
end

fn text_at(_value: Json, _steps: Text) : Option(Text)
  stop "a stand-in"
end

fn int_at(_value: Json, _steps: Text) : Option(Int)
  stop "a stand-in"
end

fn list_at(_value: Json, _steps: Text) : Option(List(Json))
  stop "a stand-in"
end
