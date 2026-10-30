module HttpAuthority
expose Authority(..), parse, check, equal?

struct Authority
  host: Text
  port: Text
end

fn hex?(c: Char) : Bool
  (c >= '0' and c <= '9') or (c >= 'a' and c <= 'f') or (c >= 'A' and c <= 'F')
end

fn ipv4?(text: Text) : Bool
  parts = text.split(".")
  return false if parts.size != 4
  for part in parts
    chars = part.chars
    return false if chars.empty? or chars.size > 3
    return false if chars.size > 1 and chars.first == Some('0')
    var value = 0
    for c in chars
      return false if c < '0' or c > '9'
      value = value * 10 + c.code - 48
    end
    return false if value > 255
  end
  true
end

fn ipv6?(text: Text) : Bool
  halves = text.split("::")
  return false if halves.size > 2
  var units = 0
  var index = 0
  parts = text.split(":")
  for part in parts
    if not part.empty?
      if part.contains?(".")
        return false if index != parts.size - 1 or not ipv4?(part)
        units += 2
      else
        return false if part.char_count > 4 or not part.chars.all?(hex?)
        units += 1
      end
    else if halves.size != 2
      return false
    end
    index += 1
  end
  if halves.size == 2
    left = halves.get(0) or ""
    right = halves.get(1) or ""
    return false if left.starts_with?(":") or
      left.ends_with?(":") or
      right.starts_with?(":") or
      right.ends_with?(":")
    units < 8
  else
    units == 8
  end
end

fn reg_name?(chars: List(Char)) : Bool
  return false if chars.empty?
  var at = 0
  while at < chars.size
    c = chars.get(at) or ' '
    if c == '%'
      return false if not hex?(chars.get(at + 1) or ' ') or not hex?(chars.get(at + 2) or ' ')
      at += 3
    else
      return false if not (
        (c >= 'a' and c <= 'z') or (c >= 'A' and c <= 'Z') or (c >= '0' and c <= '9') or
          "-._~!$&'()*+,;=".chars.contains?(c)
      )
      at += 1
    end
  end
  true
end

fn parse(text: Text) : Option(Authority)
  chars = text.chars
  return None if chars.empty?
  var end_host = 0
  var host = ""
  if chars.first == Some('[')
    while end_host < chars.size and chars.get(end_host) != Some(']')
      end_host += 1
    end
    return None if end_host >= chars.size
    inside = Text.from_chars(chars.take(end_host).drop(1))
    return None if not ipv6?(inside)
    end_host += 1
    host = Text.from_chars(chars.take(end_host))
  else
    while end_host < chars.size and chars.get(end_host) != Some(':')
      end_host += 1
    end
    name = chars.take(end_host)
    return None if not reg_name?(name)
    host = Text.from_chars(name)
  end
  var port = "80"
  if end_host < chars.size
    return None if chars.get(end_host) != Some(':')
    digits = chars.drop(end_host + 1)
    for c in digits
      return None if c < '0' or c > '9'
    end
    if not digits.empty?
      var start = 0
      while start < digits.size - 1 and digits.get(start) == Some('0')
        start += 1
      end
      port = Text.from_chars(digits.drop(start))
    end
  end
  Some(Authority(host: host, port: port))
end

fn check(authorities: List(Text)) : Bool
  authorities.all?(fn(text) parse(text) != None end)
end

fn equal?(left: Authority, right: Authority) : Bool
  return false if left.port != right.port
  a = left.host.chars
  b = right.host.chars
  return false if a.size != b.size
  for at in 0..a.size
    x = (a.get(at) or ' ').code
    y = (b.get(at) or ' ').code
    lower_x = if x >= 65 and x <= 90: x + 32 else: x
    lower_y = if y >= 65 and y <= 90: y + 32 else: y
    return false if lower_x != lower_y
  end
  true
end
