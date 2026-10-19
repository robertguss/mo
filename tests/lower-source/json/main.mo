module Main
use Json

fn main(console: Console)
  case Json.parse("{\"a\": [1, 2.5e1, \"\\u00e9\"], \"b\": null}")
    Ok(value):
      console.print(value.to_text)
      if value is Json.Object(fields)
        if fields.get("a") is Some(Json.Array([Json.Number(one), Json.Number(big), _]))
          console.print("#{one.to_int}/#{big.to_int}/#{big.to_decimal}/#{big}")
        end
      end
    Error(e): console.print("#{e}")
  end
  console.print("#{Json.parse("[tru]")}")
  console.print(Json.Array([Json.int(1), Json.Object({"k": Json.String("v")})]).pretty)
end
