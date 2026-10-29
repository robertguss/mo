module McpScalar
expose Value(..), decode, encode
use Json
use McpNumber
use McpType

enum Value
  UnitValue
  BoolValue(Bool)
  ByteValue(Byte)
  IntValue(Int)
  FloatValue(Float)
  CharValue(Char)
  TimeValue(Time)
  DurationValue(Int)
  DecimalValue(Decimal)
  TextValue(Text)
  BytesValue(List(Byte))
  BigIntValue(BigInt)
  RangeValue(start: Int, finish: Int)
end

fn decode(kind: McpType.Kind, json: Json.Json, bigint_digits: Int) : Option(Value)
  case (kind, json)
    (McpType.UnitType, Json.Null): Some(UnitValue)
    (McpType.BoolType, Json.Boolean(value)): Some(BoolValue(value))
    (McpType.ByteType, _): McpNumber.byte(json).map(fn(value) ByteValue(value) end)
    (McpType.IntType, Json.String(text)): text.to_int.map(fn(value) IntValue(value) end)
    (McpType.FloatType, Json.String(text)): text.to_float.map(fn(value) FloatValue(value) end)
    (McpType.CharType, Json.String(text)):
      chars = text.chars
      if chars.size == 1
        chars.get(0).map(fn(value) CharValue(value) end)
      else
        None
      end
    (McpType.TimeType, Json.String(text)): Time.parse(text).map(fn(value) TimeValue(value) end)
    (McpType.DurationType, Json.String(text)): text.to_int.map(fn(value) DurationValue(value) end)
    (McpType.DecimalType, Json.String(text)): text.to_decimal.map(fn(value) DecimalValue(value) end)
    (McpType.TextType, Json.String(text)): Some(TextValue(text))
    (McpType.BytesType, Json.Array(items)):
      var bytes = []
      for item in items
        case McpNumber.byte(item)
          Some(value): bytes = bytes.append(value)
          None: return None
        end
      end
      Some(BytesValue(bytes))
    (McpType.BigIntType, Json.String(text)): McpNumber.bigint(text, bigint_digits).map(
      fn(value) BigIntValue(value) end
    )
    (McpType.RangeType, Json.Object(fields)):
      return None if fields.size != 2
      case (fields.get("start"), fields.get("end"))
        (Some(Json.String(start)), Some(Json.String(finish))):
          case (start.to_int, finish.to_int)
            (Some(a), Some(b)): Some(RangeValue(start: a, finish: b))
            _: None
          end
        _: None
      end
    _: None
  end
end

fn encode(value: Value) : Json.Json
  case value
    UnitValue: Json.Null
    BoolValue(v): Json.Boolean(v)
    ByteValue(v): Json.int(v.to_int)
    IntValue(v): Json.String(v.to_text)
    FloatValue(v): Json.String(v.to_text)
    CharValue(v): Json.String(v.to_text)
    TimeValue(v): Json.String(v.to_text)
    DurationValue(v): Json.String(v.to_text)
    DecimalValue(v): Json.String(v.to_text)
    TextValue(v): Json.String(v)
    BytesValue(v): Json.Array(v.map(fn(b) Json.int(b.to_int) end))
    BigIntValue(v): Json.String(v.to_text)
    RangeValue(start: a, finish: b): Json.Object({
      "start": Json.String(a.to_text),
      "end": Json.String(b.to_text)
    })
  end
end
