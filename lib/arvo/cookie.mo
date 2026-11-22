## RFC 6265 HTTP Cookie parsing and injection-safe `Set-Cookie` response builders.
module Arvo.Cookie
expose SameSite(..),
CookieOptions(..),
default_options,
session_options,
valid_name?,
valid_value?,
parse,
get,
set,
clear
use Api

## Browser `SameSite` policy for a cookie.
enum SameSite
  Lax
  Strict
  NoneSite
end

## Attributes controlling how a `Set-Cookie` header is emitted.
struct CookieOptions
  path: Text
  max_age: Option(Int)
  http_only?: Bool
  secure?: Bool
  same_site: SameSite
end

## Default secure `HttpOnly` cookie options (`Path=/`, `SameSite=Lax`, session lifetime).
fn default_options() : CookieOptions
  CookieOptions(path: "/", max_age: None, http_only?: true, secure?: true, same_site: Lax)
end

## Options for an `HttpOnly` session cookie with explicit `Secure` flag and `Max-Age` seconds.
fn session_options(secure?: Bool, max_age: Int) : CookieOptions
  CookieOptions(
    path: "/",
    max_age: Some(max_age),
    http_only?: true,
    secure?: secure?,
    same_site: Lax
  )
end

fn token_separator?(b: Int) : Bool
  b == 40 or
    b == 41 or
    b == 60 or
    b == 62 or
    b == 64 or
    b == 44 or
    b == 59 or
    b == 58 or
    b == 92 or
    b == 34 or
    b == 47 or
    b == 91 or
    b == 93 or
    b == 63 or
    b == 61 or
    b == 123 or
    b == 125
end

## Whether `name` is a valid RFC 6265 cookie token (non-empty ASCII token without separators or CTLs).
fn valid_name?(name: Text) : Bool
  examples
    valid_name?("arvo_session")
    valid_name?("XSRF-TOKEN")
    not valid_name?("")
    not valid_name?("bad;name")
    not valid_name?("bad\r\nname")
    not valid_name?("bad=name")
  end

  bs = name.bytes
  return false if bs.size == 0
  var i = 0
  while i < bs.size
    b = (bs.get(i) or 0).to_int
    return false if b <= 32 or b >= 127 or token_separator?(b)
    i += 1
  end
  true
end

## Whether `value` is free of control characters (`\r`, `\n`, `\0`) and `;` attribute injection.
fn valid_value?(value: Text) : Bool
  examples
    valid_value?("Zm9vYmFy")
    valid_value?("")
    not valid_value?("abc; Admin=true")
    not valid_value?("abc\r\nSet-Cookie: evil=1")
    not valid_value?("abc\u{0}def")
  end

  bs = value.bytes
  var i = 0
  while i < bs.size
    b = (bs.get(i) or 0).to_int
    return false if b < 32 or b == 127 or b == 59
    i += 1
  end
  true
end

fn valid_path?(path: Text) : Bool
  return false if not path.starts_with?("/")
  bs = path.bytes
  var i = 0
  while i < bs.size
    b = (bs.get(i) or 0).to_int
    return false if b < 32 or b == 127 or b == 59
    i += 1
  end
  true
end

## Parses all `Cookie` headers on `req` into a `Map(Text, Text)`, ignoring malformed segments.
fn parse(req: Api.Request) : Map(Text, Text)
  examples
    parse(Api.get("/").with_header("Cookie", "sid=abc; theme=dark")).get("sid") == Some("abc")
    parse(Api.get("/").with_header("Cookie", "malformed; a=1=2; =empty")).get("a") == Some("1=2")
    parse(Api.get("/")) == {}
  end

  var cookies = {}
  for raw_header in req.headers_named("cookie")
    for raw_seg in raw_header.split(";")
      seg = raw_seg.trim
      continue if seg.empty? or not seg.contains?("=")
      parts = seg.split("=")
      name = (parts.first or "").trim
      continue if not valid_name?(name)
      val = parts.drop(1).join("=").trim
      continue if not valid_value?(val)
      cookies = cookies.set(name, val)
    end
  end
  cookies
end

## Reads a single cookie by exact `name` from `req`.
fn get(req: Api.Request, name: Text) : Option(Text)
  examples
    get(Api.get("/").with_header("Cookie", "sid=xyz; mode=1"), "sid") == Some("xyz")
    get(Api.get("/").with_header("Cookie", "sid=xyz"), "missing") == None
  end

  parse(req).get(name)
end

fn same_site_label(ss: SameSite) : Text
  case ss
    Lax: "Lax"
    Strict: "Strict"
    NoneSite: "None"
  end
end

fn header_safe?(s: Text) : Bool
  not s.contains?("\r") and not s.contains?("\n") and not s.contains?("\u{0}")
end

## Appends a `Set-Cookie` header to `res` without clobbering existing `Set-Cookie` headers.
## Rejects CR/LF/NUL/semicolon injection in cookie names, values, or paths by leaving `res` unchanged.
fn set(res: Api.Response, name: Text, value: Text, opts: CookieOptions) : Api.Response
  promise "emitted Set-Cookie headers never contain CR, LF, or NUL"
    result.headers.drop(res.headers.size).all?(fn(h) header_safe?(h.1) end)
  end

  return res if not valid_name?(name) or not valid_value?(value)
  clean_path = if opts.path.empty?: "/" else: opts.path
  return res if not valid_path?(clean_path)

  age_part = case opts.max_age
    Some(seconds): "; Max-Age=#{seconds}"
    None: ""
  end
  http_only_part = if opts.http_only?: "; HttpOnly" else: ""
  secure_part = if opts.secure?: "; Secure" else: ""
  ss_part = "; SameSite=#{same_site_label(opts.same_site)}"
  line = "#{name}=#{value}; Path=#{clean_path}#{age_part}#{ss_part}#{http_only_part}#{secure_part}"
  res.add_header("Set-Cookie", line)
end

## Appends a cookie-clearing `Set-Cookie` header (`Max-Age=0` and Unix-epoch `Expires`).
fn clear(res: Api.Response, name: Text, path: Text, secure?: Bool) : Api.Response
  promise "clearing Set-Cookie headers never contain CR, LF, or NUL"
    result.headers.drop(res.headers.size).all?(fn(h) header_safe?(h.1) end)
  end

  return res if not valid_name?(name)
  clean_path = if path.empty?: "/" else: path
  return res if not valid_path?(clean_path)

  secure_part = if secure?: "; Secure" else: ""
  line = "#{name}=; Path=#{clean_path}; Max-Age=0; Expires=Thu, 01 Jan 1970 00:00:00 GMT; SameSite=Lax; HttpOnly#{secure_part}"
  res.add_header("Set-Cookie", line)
end

test "parse reads multiple Cookie headers and ignores malformed segments"
  req = Api.get("/").add_header("Cookie", "  arvo_session = tok_123 ; garbage ; =nameless ; a=b=c ")
    .add_header("cookie", "XSRF-TOKEN=csrf_456; bad\u{0}=x")
  parsed = parse(req)
  assert parsed.get("arvo_session") == Some("tok_123")
  assert parsed.get("XSRF-TOKEN") == Some("csrf_456")
  assert parsed.get("a") == Some("b=c")
  assert parsed.get("") == None
  assert parsed.size == 3
  assert get(req, "XSRF-TOKEN") == Some("csrf_456")
end

test "set and clear preserve multiple Set-Cookie headers and block injection"
  r0 = Api.text_response(200, "ok")
  r1 = set(r0, "arvo_session", "s1", session_options(true, 3600))
  r2 = set(
    r1,
    "XSRF-TOKEN",
    "c1",
    CookieOptions(path: "/", max_age: None, http_only?: false, secure?: true, same_site: Strict)
  )
  cookies = r2.headers_named("set-cookie")
  assert cookies == [
    "arvo_session=s1; Path=/; Max-Age=3600; SameSite=Lax; HttpOnly; Secure",
    "XSRF-TOKEN=c1; Path=/; SameSite=Strict; Secure"
  ]

  r3 = clear(r0, "arvo_session", "/", false)
  assert r3.headers_named("set-cookie") == [
    "arvo_session=; Path=/; Max-Age=0; Expires=Thu, 01 Jan 1970 00:00:00 GMT; SameSite=Lax; HttpOnly"
  ]

  assert set(r0, "bad;name", "v", default_options()).headers_named("set-cookie") == []
  assert set(r0, "sid", "v; Max-Age=999999", default_options()).headers_named("set-cookie") == []
  assert set(r0, "sid", "v\r\nX-Evil: 1", default_options()).headers_named("set-cookie") == []
  assert set(r0, "sid", "v\nX-Evil: 1", default_options()).headers_named("set-cookie") == []
  assert set(r0, "sid", "v\u{0}tail", default_options()).headers_named("set-cookie") == []
  assert set(
    r0,
    "sid",
    "ok",
    CookieOptions(path: "/a;Evil", max_age: None, http_only?: true, secure?: false, same_site: Lax)
  ).headers_named("set-cookie") == []
  assert clear(r0, "sid", "relative", true).headers_named("set-cookie") == []
end
