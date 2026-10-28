module McpSchema
expose input, output
use Json
use McpType

struct Definitions
  names: Map(Int, Text)
  schemas: Map(Text, Json.Json)
end

fn exact(fields: List((Text, Json.Json))) : Json.Json
  var properties = {}
  var required = []
  for (name, schema) in fields
    properties = properties.set(name, schema)
    required = required.append(Json.String(name))
  end
  Json.Object({
    "type": Json.String("object"),
    "properties": Json.Object(properties),
    "required": Json.Array(required),
    "additionalProperties": Json.Boolean(false)
  })
end

fn typed(name: Text) : Json.Json
  Json.Object({"type": Json.String(name)})
end

fn patterned(pattern: Text) : Json.Json
  Json.Object({"type": Json.String("string"), "pattern": Json.String(pattern)})
end

fn integer() : Json.Json
  patterned("^-?[0-9]+$")
end

fn byte() : Json.Json
  Json.Object({"type": Json.String("integer"), "minimum": Json.int(0), "maximum": Json.int(255)})
end

fn array(item: Json.Json) : Json.Json
  Json.Object({"type": Json.String("array"), "items": item})
end

fn tuple(items: List(Json.Json)) : Json.Json
  var properties = {
    "type": Json.String("array"),
    "minItems": Json.int(items.size),
    "maxItems": Json.int(items.size),
    "items": Json.Boolean(false)
  }
  if not items.empty?
    properties = properties.set("prefixItems", Json.Array(items))
  end
  Json.Object(properties)
end

fn constant(value: Text) : Json.Json
  Json.Object({"const": Json.String(value)})
end

fn enumeration(values: List(Text)) : Json.Json
  Json.Object({"enum": Json.Array(values.map(fn(value) Json.String(value) end))})
end

fn fields(types: List(McpType.Kind), members: List(McpType.Field), definitions: Definitions) : (
  Json.Json,
  Definitions
)
  var state = definitions
  var properties = []
  for field in members
    (schema, after) = at(types, field.type_index, state)
    state = after
    properties = properties.append((field.name, schema))
  end
  (exact(properties), state)
end

fn choices(types: List(McpType.Kind), cases: List(McpType.Case), definitions: Definitions) : (
  Json.Json,
  Definitions
)
  var state = definitions
  var branches = []
  for variant in cases
    var properties = [("case", constant(variant.name))]
    case variant.payload
      McpType.Empty: Unit
      McpType.Unnamed(index):
        (schema, after) = at(types, index, state)
        state = after
        properties = properties.append(("value", schema))
      McpType.Named(members):
        (schema, after) = fields(types, members, state)
        state = after
        properties = properties.append(("fields", schema))
    end
    branches = branches.append(exact(properties))
  end
  if branches.empty?
    (Json.Boolean(false), state)
  else
    (Json.Object({"oneOf": Json.Array(branches)}), state)
  end
end

fn at(types: List(McpType.Kind), index: Int, definitions: Definitions) : (Json.Json, Definitions)
  kind = case types.get(index)
    Some(value): value
    None: stop "MCP descriptor index"
  end
  var state = definitions
  case kind
    McpType.StructType(_) | McpType.ChoiceType(_):
      case state.names.get(index)
        Some(name): return (Json.Object({"$ref": Json.String("#/$defs/#{name}")}), state)
        None: Unit
      end
      name = "t#{state.names.size}"
      state.names = state.names.set(index, name)
      (schema, after) = body(types, kind, state)
      state = after
      state.schemas = state.schemas.set(name, schema)
      (Json.Object({"$ref": Json.String("#/$defs/#{name}")}), state)
    McpType.UnitType | McpType.BoolType | McpType.ByteType | McpType.IntType | McpType.FloatType | McpType
      .CharType | McpType.TimeType | McpType.DurationType | McpType.DecimalType | McpType.TextType | McpType
      .BytesType | McpType.BigIntType | McpType.TupleType(_) | McpType.ListType(_) | McpType.SetType(
        _
      ) | McpType.MapType(key: _, value: _) | McpType.RangeType: body(types, kind, state)
  end
end

fn body(types: List(McpType.Kind), kind: McpType.Kind, definitions: Definitions) : (
  Json.Json,
  Definitions
)
  var state = definitions
  schema = case kind
    McpType.UnitType: typed("null")
    McpType.BoolType: typed("boolean")
    McpType.ByteType: byte()
    McpType.IntType | McpType.BigIntType | McpType.DurationType: integer()
    McpType.FloatType: patterned("^(-?[0-9]+(\\.[0-9]+)?([eE][+-]?[0-9]+)?|nan|inf|-inf)$")
    McpType.DecimalType: patterned("^-?[0-9]+(\\.[0-9]{1,18})?$")
    McpType.CharType: Json.Object({
      "type": Json.String("string"),
      "minLength": Json.int(1),
      "maxLength": Json.int(1)
    })
    McpType.TimeType: Json.Object({
      "type": Json.String("string"),
      "format": Json.String("date-time"),
      "pattern": Json.String(
        "^[0-9]{4}-[0-9]{2}-[0-9]{2}[Tt][0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]{1,6})?([Zz]|[+-][0-9]{2}:[0-9]{2})$"
      )
    })
    McpType.TextType: typed("string")
    McpType.BytesType: array(byte())
    McpType.RangeType: exact([("start", integer()), ("end", integer())])
    McpType.TupleType(indices):
      var items = []
      for index in indices
        (item, after) = at(types, index, state)
        state = after
        items = items.append(item)
      end
      tuple(items)
    McpType.ListType(index) | McpType.SetType(index):
      (item, after) = at(types, index, state)
      state = after
      array(item)
    McpType.MapType(key: key, value: value):
      (key_schema, after_key) = at(types, key, state)
      (value_schema, after_value) = at(types, value, after_key)
      state = after_value
      array(tuple([key_schema, value_schema]))
    McpType.StructType(members): return fields(types, members, state)
    McpType.ChoiceType(cases): return choices(types, cases, state)
  end
  (schema, state)
end

fn finish(schema: Json.Json, definitions: Definitions) : Json.Json
  var ordered = {}
  for index in 0..definitions.names.size
    name = "t#{index}"
    case definitions.schemas.get(name)
      Some(value): ordered = ordered.set(name, value)
      None: stop "MCP schema definition"
    end
  end
  case schema
    Json.Object(properties):
      var result = {"$schema": Json.String("https://json-schema.org/draft/2020-12/schema")}
      for (name, value) in properties
        result = result.set(name, value)
      end
      if not ordered.empty?
        result = result.set("$defs", Json.Object(ordered))
      end
      Json.Object(result)
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): stop "MCP schema root"
  end
end

fn input(types: List(McpType.Kind), arguments: List(McpType.Field)) : Json.Json
  (schema, definitions) = fields(types, arguments, Definitions(names: {}, schemas: {}))
  finish(schema, definitions)
end

fn output(types: List(McpType.Kind), reply: Int, error: Option(Int)) : Json.Json
  (value, initial) = at(types, reply, Definitions(names: {}, schemas: {}))
  var state = initial
  var branches = [exact([("value", value)])]
  case error
    Some(index):
      (failure, after) = at(types, index, state)
      state = after
      branches = branches.append(
        exact([("error", exact([("kind", constant("application")), ("value", failure)]))])
      )
    None: Unit
  end
  branches = branches.append(
    exact([
      (
        "error",
        exact([
          ("kind", constant("runtime")),
          (
            "code",
            enumeration([
              "invalid_arguments",
              "busy",
              "stopped",
              "memory_limit",
              "work_limit",
              "handler_failed",
              "store_failed",
              "rate_limited",
              "response_limit",
              "internal_error"
            ])
          ),
          ("message", typed("string")),
          ("effect", enumeration(["none", "applied", "unknown"]))
        ])
      )
    ])
  )
  finish(Json.Object({"type": Json.String("object"), "oneOf": Json.Array(branches)}), state)
end
