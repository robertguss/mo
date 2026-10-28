module McpSchemaCheck
use Json
use McpSchema
use McpType

fn require(ok: Bool)
  if not ok
    stop "MCP schema contract"
  end
end

fn json(text: Text) : Json.Json
  case Json.parse(text)
    Ok(value): value
    Error(_): stop "MCP schema fixture"
  end
end

fn member(value: Json.Json, name: Text) : Json.Json
  case value
    Json.Object(properties):
      case properties.get(name)
        Some(item): item
        None: stop "MCP schema property"
      end
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): stop "MCP schema object"
  end
end

fn scalars()
  for (kind, expected) in [
    (McpType.UnitType, "{\"type\":\"null\"}"),
    (McpType.BoolType, "{\"type\":\"boolean\"}"),
    (McpType.ByteType, "{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}"),
    (McpType.TextType, "{\"type\":\"string\"}"),
    (McpType.CharType, "{\"type\":\"string\",\"minLength\":1,\"maxLength\":1}"),
    (McpType.IntType, "{\"type\":\"string\",\"pattern\":\"^-?[0-9]+$\"}"),
    (McpType.BigIntType, "{\"type\":\"string\",\"pattern\":\"^-?[0-9]+$\"}"),
    (McpType.DurationType, "{\"type\":\"string\",\"pattern\":\"^-?[0-9]+$\"}"),
    (McpType.DecimalType, "{\"type\":\"string\",\"pattern\":\"^-?[0-9]+(\\\\.[0-9]{1,18})?$\"}"),
    (
      McpType.TimeType,
      "{\"type\":\"string\",\"format\":\"date-time\",\"pattern\":\"^[0-9]{4}-[0-9]{2}-[0-9]{2}[Tt][0-9]{2}:[0-9]{2}:[0-9]{2}(\\\\.[0-9]{1,6})?([Zz]|[+-][0-9]{2}:[0-9]{2})$\"}"
    ),
    (
      McpType.FloatType,
      "{\"type\":\"string\",\"pattern\":\"^(-?[0-9]+(\\\\.[0-9]+)?([eE][+-]?[0-9]+)?|nan|inf|-inf)$\"}"
    )
  ]
    result = McpSchema.input([kind], [McpType.Field(name: "_value", type_index: 0)])
    require(member(member(result, "properties"), "_value") == json(expected))
    require(member(result, "required") == json("[\"_value\"]"))
    require(member(result, "additionalProperties") == Json.Boolean(false))
  end
end

fn containers()
  for (kind, expected) in [
    (McpType.ListType(1), "{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"),
    (McpType.SetType(1), "{\"type\":\"array\",\"items\":{\"type\":\"string\"}}"),
    (McpType.TupleType([]), "{\"type\":\"array\",\"minItems\":0,\"maxItems\":0,\"items\":false}"),
    (
      McpType.TupleType([1, 2]),
      """
      {"type":"array","minItems":2,"maxItems":2,"items":false,
      "prefixItems":[{"type":"string"},{"type":"boolean"}]}
      """
    ),
    (
      McpType.MapType(key: 1, value: 2),
      """
      {"type":"array","items":{"type":"array","minItems":2,"maxItems":2,"items":false,
      "prefixItems":[{"type":"string"},{"type":"boolean"}]}}
      """
    ),
    (
      McpType.BytesType,
      """
      {"type":"array","items":{"type":"integer","minimum":0,"maximum":255}}
      """
    ),
    (
      McpType.RangeType,
      """
      {"type":"object","properties":{"start":{"type":"string","pattern":"^-?[0-9]+$"},
      "end":{"type":"string","pattern":"^-?[0-9]+$"}},
      "required":["start","end"],"additionalProperties":false}
      """
    )
  ]
    result = McpSchema.input(
      [kind, McpType.TextType, McpType.BoolType],
      [McpType.Field(name: "x", type_index: 0)]
    )
    require(member(member(result, "properties"), "x") == json(expected))
  end
  for (kind, expected) in [
    (
      McpType.StructType([]),
      """
      {"type":"object","properties":{},"required":[],"additionalProperties":false}
      """
    ),
    (McpType.ChoiceType([]), "false"),
    (
      McpType.ChoiceType([
        McpType.Case(
          name: "Value",
          payload: McpType.Named([McpType.Field(name: "0", type_index: 1)])
        )
      ]),
      """
      {"oneOf":[{"type":"object","properties":{"case":{"const":"Value"},
      "fields":{"type":"object","properties":{"0":{"type":"string"}},
      "required":["0"],"additionalProperties":false}},
      "required":["case","fields"],"additionalProperties":false}]}
      """
    )
  ]
    result = McpSchema.input([kind, McpType.TextType], [McpType.Field(name: "x", type_index: 0)])
    require(member(member(result, "$defs"), "t0") == json(expected))
  end
end

fn main() : Int
  scalars()
  containers()
  types = [
    McpType.TextType,
    McpType.ChoiceType([
      McpType.Case(name: "End", payload: McpType.Empty),
      McpType.Case(name: "Next", payload: McpType.Unnamed(2))
    ]),
    McpType.StructType([McpType.Field(name: "next", type_index: 1)]),
    McpType.StructType([McpType.Field(name: "next", type_index: 1)])
  ]
  arguments = [
    McpType.Field(name: "_first", type_index: 2),
    McpType.Field(name: "second", type_index: 2),
    McpType.Field(name: "distinct", type_index: 3)
  ]
  expected = json(
    """
    {"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object",
    "properties":{"_first":{"$ref":"#/$defs/t0"},"second":{"$ref":"#/$defs/t0"},
    "distinct":{"$ref":"#/$defs/t2"}},
    "required":["_first","second","distinct"],"additionalProperties":false,
    "$defs":{"t0":{"type":"object","properties":{"next":{"$ref":"#/$defs/t1"}},
    "required":["next"],"additionalProperties":false},
    "t1":{"oneOf":[{"type":"object","properties":{"case":{"const":"End"}},
    "required":["case"],"additionalProperties":false},
    {"type":"object","properties":{"case":{"const":"Next"},"value":{"$ref":"#/$defs/t0"}},
    "required":["case","value"],"additionalProperties":false}]},
    "t2":{"type":"object","properties":{"next":{"$ref":"#/$defs/t1"}},
    "required":["next"],"additionalProperties":false}}}
    """
  )
  require(McpSchema.input(types, arguments).to_text == expected.to_text)
  output = McpSchema.output(types, 0, Some(0))
  require(member(output, "type") == Json.String("object"))
  require(
    member(output, "oneOf") ==
      json(
        """
    [{"type":"object","properties":{"value":{"type":"string"}},
      "required":["value"],"additionalProperties":false},
     {"type":"object","properties":{"error":{"type":"object",
       "properties":{"kind":{"const":"application"},"value":{"type":"string"}},
       "required":["kind","value"],"additionalProperties":false}},
      "required":["error"],"additionalProperties":false},
     {"type":"object","properties":{"error":{"type":"object",
       "properties":{"kind":{"const":"runtime"},"code":{"enum":["invalid_arguments",
       "busy","stopped","memory_limit","work_limit","handler_failed","store_failed",
       "rate_limited","response_limit","internal_error"]},"message":{"type":"string"},
       "effect":{"enum":["none","applied","unknown"]}},
       "required":["kind","code","message","effect"],"additionalProperties":false}},
      "required":["error"],"additionalProperties":false}]
    """
      )
  )
  case member(output, "oneOf")
    Json.Array(branches): require(
      member(McpSchema.output(types, 0, None), "oneOf") ==
        Json.Array(branches.take(1).concat(branches.drop(2)))
    )
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Object(_): stop "MCP output branches"
  end
  37
end
