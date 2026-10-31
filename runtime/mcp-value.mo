module McpValue
expose Value(..), Problem(..), decode, encode, arguments
use Json
use McpScalar
use McpType

enum Value
  Leaf(McpScalar.Value)
  Sequence(List(Value))
  Variant(index: Int, fields: List(Value))
end

enum Problem
  Invalid
  Limit
end

fn decode(types: List(McpType.Kind), index: Int, json: Json.Json, depth: Int, digits: Int) : Result(
  Value,
  Problem
)
  return Error(Limit) if depth <= 0
  kind = types.get(index) or stop "MCP descriptor index"
  case (kind, json)
    (McpType.StructType(fields), _):
      values = try arguments(types, fields, json, depth - 1, digits)
      Ok(Sequence(values))
    (McpType.ChoiceType(cases), Json.Object(fields)):
      name = case fields.get("case")
        Some(Json.String(value)): value
        None | Some(_): return Error(Invalid)
      end
      for i in 0..cases.size
        variant = cases.get(i) or stop "MCP case index"
        if variant.name == name
          values = case variant.payload
            McpType.Empty:
              return Error(Invalid) if fields.size != 1
              []
            McpType.Unnamed(child):
              return Error(Invalid) if fields.size != 2
              input = try fields.get("value").or_fail(Invalid)
              value = try decode(types, child, input, depth - 1, digits)
              [value]
            McpType.Named(members):
              return Error(Invalid) if fields.size != 2
              input = try fields.get("fields").or_fail(Invalid)
              try arguments(types, members, input, depth - 1, digits)
          end
          return Ok(Variant(index: i, fields: values))
        end
      end
      Error(Invalid)
    (McpType.TupleType(indices), Json.Array(items)):
      return Error(Invalid) if indices.size != items.size
      var values = []
      for (child, item) in indices.zip(items)
        value = try decode(types, child, item, depth - 1, digits)
        values = values.append(value)
      end
      Ok(Sequence(values))
    (McpType.ListType(child), Json.Array(items)) | (McpType.SetType(child), Json.Array(items)):
      var values = []
      for item in items
        value = try decode(types, child, item, depth - 1, digits)
        values = values.append(value)
      end
      Ok(Sequence(values))
    (McpType.MapType(key: key, value: item), Json.Array(pairs)):
      var values = []
      for pair in pairs
        case pair
          Json.Array(parts):
            return Error(Invalid) if parts.size != 2
            a = parts.get(0) or stop "MCP pair index"
            b = parts.get(1) or stop "MCP pair index"
            k = try decode(types, key, a, depth - 1, digits)
            v = try decode(types, item, b, depth - 1, digits)
            values = values.append(k).append(v)
          Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Object(_): return Error(
            Invalid
          )
        end
      end
      Ok(Sequence(values))
    _:
      case McpScalar.decode(kind, json, digits)
        Some(value): Ok(Leaf(value))
        None: Error(Invalid)
      end
  end
end

fn encode(types: List(McpType.Kind), index: Int, value: Value, depth: Int) : Result(
  Json.Json,
  Problem
)
  return Error(Limit) if depth <= 0
  kind = types.get(index) or stop "MCP descriptor index"
  case (kind, value)
    (McpType.StructType(fields), Sequence(values)): named(types, fields, values, depth - 1)
    (McpType.ChoiceType(cases), Variant(index: tag, fields: values)):
      variant = try cases.get(tag).or_fail(Invalid)
      var fields = {"case": Json.String(variant.name)}
      case variant.payload
        McpType.Empty: return Error(Invalid) if not values.empty?
        McpType.Unnamed(child):
          return Error(Invalid) if values.size != 1
          v = values.get(0) or stop "MCP payload index"
          json = try encode(types, child, v, depth - 1)
          fields = fields.set("value", json)
        McpType.Named(members):
          json = try named(types, members, values, depth - 1)
          fields = fields.set("fields", json)
      end
      Ok(Json.Object(fields))
    (McpType.TupleType(indices), Sequence(values)):
      return Error(Invalid) if indices.size != values.size
      var items = []
      for (child, v) in indices.zip(values)
        json = try encode(types, child, v, depth - 1)
        items = items.append(json)
      end
      Ok(Json.Array(items))
    (McpType.ListType(child), Sequence(values)) | (McpType.SetType(child), Sequence(values)):
      var items = []
      for v in values
        json = try encode(types, child, v, depth - 1)
        items = items.append(json)
      end
      Ok(Json.Array(items))
    (McpType.MapType(key: key, value: item), Sequence(values)):
      return Error(Invalid) if values.size % 2 != 0
      var pairs = []
      for i in 0..values.size / 2
        a = values.get(i * 2) or stop "MCP pair index"
        b = values.get(i * 2 + 1) or stop "MCP pair index"
        k = try encode(types, key, a, depth - 1)
        v = try encode(types, item, b, depth - 1)
        pairs = pairs.append(Json.Array([k, v]))
      end
      Ok(Json.Array(pairs))
    (_, Leaf(leaf)): Ok(McpScalar.encode(leaf))
    _: Error(Invalid)
  end
end

fn arguments(
  types: List(McpType.Kind),
  fields: List(McpType.Field),
  json: Json.Json,
  depth: Int,
  digits: Int
) : Result(List(Value), Problem)
  input = case json
    Json.Object(values): values
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): return Error(
      Invalid
    )
  end
  return Error(Invalid) if fields.size != input.size
  var values = []
  for field in fields
    item = try input.get(field.name).or_fail(Invalid)
    value = try decode(types, field.type_index, item, depth, digits)
    values = values.append(value)
  end
  Ok(values)
end

fn named(types: List(McpType.Kind), fields: List(McpType.Field), values: List(Value), depth: Int) : Result(
  Json.Json,
  Problem
)
  return Error(Invalid) if fields.size != values.size
  var properties = {}
  for (field, value) in fields.zip(values)
    json = try encode(types, field.type_index, value, depth)
    properties = properties.set(field.name, json)
  end
  Ok(Json.Object(properties))
end
