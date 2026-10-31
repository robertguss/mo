module Main
use Json
use McpScalar
use McpType
use McpValue

fn require(ok: Bool)
  if not ok
    stop "MCP composite contract"
  end
end

fn json(text: Text) : Json.Json
  checked(Json.parse(text))
end

fn checked(value: Result(T, E)) : T
  case value
    Ok(result): result
    Error(_): stop "MCP composite acceptance"
  end
end

fn main() : Int
  types = [
    McpType.IntType,
    McpType.TextType,
    McpType.ListType(0),
    McpType.TupleType([0, 1]),
    McpType.SetType(0),
    McpType.MapType(key: 0, value: 1),
    McpType.StructType([
      McpType.Field(name: "_x", type_index: 0),
      McpType.Field(name: "label", type_index: 1)
    ]),
    McpType.ChoiceType([
      McpType.Case(name: "Empty", payload: McpType.Empty),
      McpType.Case(name: "One", payload: McpType.Unnamed(0)),
      McpType.Case(
        name: "Named",
        payload: McpType.Named([McpType.Field(name: "amount", type_index: 0)])
      ),
      McpType.Case(
        name: "Pair",
        payload: McpType.Named([
          McpType.Field(name: "a", type_index: 0),
          McpType.Field(name: "b", type_index: 1)
        ])
      )
    ]),
    McpType.StructType([]),
    McpType.ChoiceType([]),
    McpType.ChoiceType([
      McpType.Case(name: "End", payload: McpType.Empty),
      McpType.Case(name: "Next", payload: McpType.Unnamed(10))
    ]),
    McpType.TupleType([]),
    McpType.ListType(6)
  ]
  for (index, input, output) in [
    (2, "[\"01\",\"-2\"]", "[\"1\",\"-2\"]"),
    (3, "[\"-3\",\"hi\"]", "[\"-3\",\"hi\"]"),
    (4, "[\"01\",\"1\"]", "[\"1\",\"1\"]"),
    (5, "[[\"01\",\"a\"],[\"1\",\"b\"]]", "[[\"1\",\"a\"],[\"1\",\"b\"]]"),
    (6, "{\"label\":\"v\",\"_x\":\"07\"}", "{\"_x\":\"7\",\"label\":\"v\"}"),
    (7, "{\"case\":\"Empty\"}", "{\"case\":\"Empty\"}"),
    (7, "{\"case\":\"One\",\"value\":\"-03\"}", "{\"case\":\"One\",\"value\":\"-3\"}"),
    (
      7,
      "{\"case\":\"Named\",\"fields\":{\"amount\":\"007\"}}",
      "{\"case\":\"Named\",\"fields\":{\"amount\":\"7\"}}"
    ),
    (
      7,
      "{\"case\":\"Pair\",\"fields\":{\"b\":\"v\",\"a\":\"8\"}}",
      "{\"case\":\"Pair\",\"fields\":{\"a\":\"8\",\"b\":\"v\"}}"
    ),
    (8, "{}", "{}"),
    (11, "[]", "[]"),
    (12, "[{\"_x\":\"1\",\"label\":\"x\"}]", "[{\"_x\":\"1\",\"label\":\"x\"}]")
  ]
    value = checked(McpValue.decode(types, index, json(input), 60, 4096))
    output_value = checked(McpValue.encode(types, index, value, 60))
    require(output_value == json(output))
  end
  for (index, input) in [
    (2, "{}"),
    (3, "[\"1\"]"),
    (3, "[\"1\",\"s\",null]"),
    (5, "[[\"1\"]]"),
    (5, "[[\"1\",\"s\",null]]"),
    (5, "{\"1\":\"s\"}"),
    (6, "{\"_x\":\"1\"}"),
    (6, "{\"_x\":\"1\",\"label\":\"s\",\"extra\":null}"),
    (7, "{\"case\":\"Missing\"}"),
    (7, "{\"case\":\"Empty\",\"value\":null}"),
    (7, "{\"case\":\"One\",\"fields\":{\"amount\":\"1\"}}"),
    (7, "{\"case\":\"One\"}"),
    (7, "{\"case\":\"One\",\"value\":\"1\",\"extra\":null}"),
    (7, "{\"case\":\"Named\",\"value\":\"1\"}"),
    (7, "{\"case\":\"Named\",\"fields\":{\"amount\":\"1\",\"other\":\"2\"}}"),
    (7, "{\"case\":\"Pair\",\"fields\":{\"a\":\"1\"}}"),
    (8, "{\"extra\":null}"),
    (9, "{\"case\":\"None\"}"),
    (11, "[null]"),
    (12, "[{\"_x\":\"1\",\"label\":\"x\",\"extra\":null}]")
  ]
    require(McpValue.decode(types, index, json(input), 60, 4096) == Error(McpValue.Invalid))
  end
  var nested = json("{\"case\":\"End\"}")
  for _level in 1..60
    nested = Json.Object({"case": Json.String("Next"), "value": nested})
  end
  value = checked(McpValue.decode(types, 10, nested, 60, 4096))
  require(McpValue.encode(types, 10, value, 60) == Ok(nested))
  require(McpValue.decode(types, 10, nested, 59, 4096) == Error(McpValue.Limit))
  require(McpValue.encode(types, 10, value, 59) == Error(McpValue.Limit))
  fields = [McpType.Field(name: "_arg", type_index: 0)]
  require(
    McpValue.arguments(types, fields, json("{\"_arg\":\"07\"}"), 60, 4096) ==
      Ok([McpValue.Leaf(McpScalar.IntValue(7))])
  )
  require(McpValue.arguments(types, fields, json("{}"), 60, 4096) == Error(McpValue.Invalid))
  require(
    McpValue.arguments(types, fields, json("{\"_arg\":\"7\",\"x\":null}"), 60, 4096) ==
      Error(McpValue.Invalid)
  )
  37
end
