## Double-submit + session-bound CSRF protection compatible with `@inertiajs/react` and Axios
## (`XSRF-TOKEN` readable cookie + `X-XSRF-TOKEN` request header).
module Arvo.Csrf
expose cookie_name, header_name, safe_method?, verify, verify_token, attach_cookie, clear_cookie
use Api
use Arvo.Cookie
use Arvo.Session
use Crypto

cookie_name = "XSRF-TOKEN"
header_name = "X-XSRF-TOKEN"

## Whether `req.method` is an HTTP safe read method (`GET`, `HEAD`, or `OPTIONS`).
fn safe_method?(req: Api.Request) : Bool
  examples
    safe_method?(Api.get("/"))
    safe_method?(Api.request("HEAD", "/"))
    safe_method?(Api.request("OPTIONS", "/"))
    not safe_method?(Api.post("/"))
    not safe_method?(Api.delete("/"))
  end

  m = req.method.ascii_uppercase
  m == "GET" or m == "HEAD" or m == "OPTIONS"
end

## Constant-time verification of an explicit CSRF `token` (e.g. form `_csrf`) against `session.csrf_token`.
fn verify_token(token: Text, session: Session.Session) : Bool
  return false if token.empty? or session.csrf_token.empty?
  Crypto.constant_time_eq?(token.bytes, session.csrf_token.bytes)
end

## Verifies `X-XSRF-TOKEN` on `req` (percent-decoding if URL-encoded) against `session.csrf_token`
## in constant time.
fn verify(req: Api.Request, session: Session.Session) : Bool
  raw = case req.header(header_name)
    Some(v): v.trim
    None: return false
  end
  decoded = Api.percent_decode(raw, false) or raw
  verify_token(decoded, session)
end

## Attaches the readable `XSRF-TOKEN` cookie (`HttpOnly=false`, `SameSite=Lax`, `Path=/`).
fn attach_cookie(res: Api.Response, session: Session.Session, secure?: Bool) : Api.Response
  opts = Cookie.CookieOptions(
    path: "/",
    max_age: None,
    http_only?: false,
    secure?: secure?,
    same_site: Cookie.Lax
  )
  Cookie.set(res, cookie_name, session.csrf_token, opts)
end

## Clears the `XSRF-TOKEN` cookie on logout.
fn clear_cookie(res: Api.Response, secure?: Bool) : Api.Response
  Cookie.clear(res, cookie_name, "/", secure?)
end

test "CSRF verification checks X-XSRF-TOKEN in constant time and handles percent-encoding"
  exp = Time.parse("2026-11-22T00:00:00Z") or stop "time"
  sess = Session.Session(id: "sess_1", member_id: "mem_1", csrf_token: "tok_a+b/c", expires_at: exp)

  assert verify_token("tok_a+b/c", sess)
  assert not verify_token("tok_a+b/d", sess)
  assert not verify_token("", sess)

  assert not verify(Api.post("/bills"), sess)
  assert verify(Api.post("/bills").with_header("X-XSRF-TOKEN", "tok_a+b/c"), sess)
  assert verify(Api.post("/bills").with_header("x-xsrf-token", "tok_a%2Bb%2Fc"), sess)
  assert not verify(Api.post("/bills").with_header("X-XSRF-TOKEN", "wrong"), sess)

  attached = attach_cookie(Api.no_content(), sess, true)
  hdr = attached.header("Set-Cookie") or ""
  assert hdr == "XSRF-TOKEN=tok_a+b/c; Path=/; SameSite=Lax; Secure"
  assert not hdr.contains?("HttpOnly")

  cleared = clear_cookie(Api.no_content(), true)
  assert (cleared.header("Set-Cookie") or "").starts_with?("XSRF-TOKEN=; Path=/; Max-Age=0")
end
