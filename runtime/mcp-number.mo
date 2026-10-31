module McpNumber
expose byte, bigint
use Json

fn octet(bytes: Bytes, at: Int) : Int
  case bytes.get(at)
    Some(value): value.to_int
    None: -1
  end
end

fn byte(value: Json.Json) : Option(Byte)
  case value
    Json.Number(_): byte_number(value.to_text.bytes)
    Json.Null: None
    Json.Boolean(_): None
    Json.String(_): None
    Json.Array(_): None
    Json.Object(_): None
  end
end

fn byte_number(bytes: Bytes) : Option(Byte)
  negative = octet(bytes, 0) == 45
  var at = if negative: 1 else: 0
  var digits = 0
  var point = -1
  var first = -1
  var last = -1
  var first_at = 0
  var last_at = 0
  while at < bytes.size
    c = octet(bytes, at)
    if c == 69 or c == 101
      break
    else if c == 46
      point = digits
    else
      if c != 48
        if first == -1
          first = digits
          first_at = at
        end
        last = digits
        last_at = at
      end
      digits += 1
    end
    at += 1
  end
  return 0.to_byte if first == -1
  return None if negative or last - first >= 3
  if point == -1
    point = digits
  end
  var exponent = 0
  if at < bytes.size
    at += 1
    minus = octet(bytes, at) == 45
    if minus or octet(bytes, at) == 43
      at += 1
    end
    while octet(bytes, at) == 48
      at += 1
    end
    bound = bytes.size + 3
    return None if bytes.size - at > bound.to_text.byte_count
    while at < bytes.size
      exponent = exponent * 10 + octet(bytes, at) - 48
      return None if exponent > bound
      at += 1
    end
    if minus
      exponent = -exponent
    end
  end
  zeroes = point + exponent - last - 1
  size = point + exponent - first
  return None if zeroes < 0 or size < 1 or size > 3
  var result = 0
  for index in first_at..last_at + 1
    c = octet(bytes, index)
    if c != 46
      result = result * 10 + c - 48
    end
  end
  for _ in 0..zeroes
    result = result * 10
  end
  result.to_byte
end

fn bigint(text: Text, digit_limit: Int) : Option(BigInt)
  return None if digit_limit < 1 or text.byte_count == 0
  return None if text.byte_count > digit_limit and text.byte_count - 1 > digit_limit
  bytes = text.bytes
  negative = octet(bytes, 0) == 45
  start = if negative: 1 else: 0
  digits = bytes.size - start
  return None if digits == 0 or digits > digit_limit
  var result = 0.to_big
  var chunk = 0
  var scale = 1
  for at in start..bytes.size
    c = octet(bytes, at)
    return None if c < 48 or c > 57
    chunk = chunk * 10 + c - 48
    scale = scale * 10
    if scale == 1000000000
      result = result * 1000000000.to_big + chunk.to_big
      chunk = 0
      scale = 1
    end
  end
  result = result * scale.to_big + chunk.to_big
  Some(if negative: -result else: result)
end
