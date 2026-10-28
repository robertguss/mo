module McpCheck
use Json
use Mcp
use McpNumber

fn require(ok: Bool)
  if not ok
    stop "MCP value contract"
  end
end

fn json(text: Text) : Json.Json
  case Json.parse(text)
    Ok(value): value
    Error(_): stop "JSON fixture"
  end
end

fn check_result(result: Json.Json, text: Text, failed: Bool)
  require(
    result ==
      Json.Object({
        "content": Json.Array([
          Json.Object({"type": Json.String("text"), "text": Json.String(text)})
        ]),
        "structuredContent": json(text),
        "isError": Json.Boolean(failed)
      })
  )
end

fn numbers()
  for (text, expected) in [
    ("255", 255),
    ("255.0", 255),
    ("2.55e2", 255),
    ("25500e-2", 255),
    ("-0", 0),
    ("-0.0", 0),
    ("0e999999", 0),
    ("-0e-999999", 0),
    ("0.00000255e8", 255),
    ("1.0200e2", 102),
    ("2e2", 200),
    ("0.010e2", 1),
    ("20", 20),
    ("2.000e1", 20),
    ("10e-1", 1),
    ("25.50e+01", 255),
    ("1e000000000000000000000000000000000000002", 100)
  ]
    require(McpNumber.byte(json(text)) == expected.to_byte)
  end
  for text in [
    "256",
    "255.5",
    "1.1",
    "25.5",
    "-1",
    "1e999999",
    "1e-999999",
    "0.00001",
    "25.01e1",
    "10001e-2",
    "1e3",
    "25600e-2",
    "-0.01e2",
    "[]",
    "{}",
    "null",
    "true",
    "\"1\""
  ]
    require(McpNumber.byte(json(text)) == None)
  end
  exponent = "9".repeat(100000)
  require(McpNumber.byte(json("1e#{exponent}")) == None)
  require(McpNumber.byte(json("0e#{exponent}")) == 0.to_byte)
  for (text, expected) in [
    ("0", 0.to_big),
    ("-000", 0.to_big),
    ("01", 1.to_big),
    ("-1234567890123456789", -1234567890123456789.to_big),
    ("999999999999999999999999999999", 10.to_big.pow(30) - 1.to_big)
  ]
    require(McpNumber.bigint(text, 30) == Some(expected))
  end
  for text in ["", "-", "+1", "1.0", "1e0", " 1", "1\n", "١", "1_0"]
    require(McpNumber.bigint(text, 30) == None)
  end
  require(McpNumber.bigint("1234567890", 10) == Some(1234567890.to_big))
  require(McpNumber.bigint("-1234567890", 10) == Some(-1234567890.to_big))
  require(McpNumber.bigint("12345678901", 10) == None)
  require(McpNumber.bigint("00000000000", 10) == None)
  require(McpNumber.bigint("1", 0) == None)
  require(McpNumber.bigint("9".repeat(4096), 4096) == Some(10.to_big.pow(4096) - 1.to_big))
  require(McpNumber.bigint("9".repeat(4097), 4096) == None)
end

fn main() : Int
  numbers()
  for (value, success, failure) in [
    (Json.Null, "{\"value\":null}", "{\"error\":{\"kind\":\"application\",\"value\":null}}"),
    (
      Json.String("a\"\\\n\t😀"),
      "{\"value\":\"a\\\"\\\\\\n\\t😀\"}",
      "{\"error\":{\"kind\":\"application\",\"value\":\"a\\\"\\\\\\n\\t😀\"}}"
    ),
    (
      json("{\"case\":\"Error\",\"value\":[false,{},\"-0\"]}"),
      "{\"value\":{\"case\":\"Error\",\"value\":[false,{},\"-0\"]}}",
      "{\"error\":{\"kind\":\"application\",\"value\":{\"case\":\"Error\",\"value\":[false,{},\"-0\"]}}}"
    )
  ]
    check_result(Mcp.success(value), success, false)
    check_result(Mcp.application_error(value), failure, true)
  end
  37
end
