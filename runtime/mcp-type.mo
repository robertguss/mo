module McpType
expose Kind(..), Field(..), Case(..), Payload(..)

enum Kind
  UnitType
  BoolType
  ByteType
  IntType
  FloatType
  CharType
  TimeType
  DurationType
  DecimalType
  TextType
  BytesType
  BigIntType
  TupleType(List(Int))
  ListType(Int)
  SetType(Int)
  MapType(key: Int, value: Int)
  RangeType
  StructType(List(Field))
  ChoiceType(List(Case))
end

struct Field
  name: Text
  type_index: Int
end

struct Case
  name: Text
  payload: Payload
end

enum Payload
  Empty
  Unnamed(Int)
  Named(List(Field))
end
