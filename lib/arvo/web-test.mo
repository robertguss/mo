## Stateful cookie, CSRF, and Inertia v2 test client wrapping `Api.Request` and `Api.Response`
## for zero-socket end-to-end tests with `ApiTest.send`.
module Arvo.WebTest
expose Client(..),
InertiaPage(..),
new,
with_inertia,
without_inertia,
with_header,
prepare,
prepare_without_csrf,
absorb,
cookie,
redirect_location,
inertia_page
use Api
use Arvo.Csrf
use Json

## Stateful browser-like test client holding a cookie jar, optional Inertia version, and default headers.
struct Client
  cookies: Map(Text, Text)
  inertia_version: Option(Text)
  default_headers: List((Text, Text))
end

## Decoded Inertia v2 page object extracted from either XHR JSON or initial HTML `data-page`.
struct InertiaPage
  component: Text
  props: Map(Text, Json)
  url: Text
  version: Text
end

## Creates an empty test client in standard HTML navigation mode.
fn new() : Client
  Client(cookies: {}, inertia_version: None, default_headers: [])
end

## Enables Inertia XHR mode (`X-Inertia: true` and `X-Inertia-Version: version`) on `client`.
fn with_inertia(client: Client, version: Text) : Client
  Client(
    cookies: client.cookies,
    inertia_version: Some(version),
    default_headers: client.default_headers
  )
end

## Disables Inertia XHR mode on `client` (simulating a full browser document request).
fn without_inertia(client: Client) : Client
  Client(cookies: client.cookies, inertia_version: None, default_headers: client.default_headers)
end

## Sets or replaces a default request header on `client` (case-insensitively).
fn with_header(client: Client, name: Text, value: Text) : Client
  low = name.ascii_lowercase
  kept = client.default_headers.filter(fn(h) h.0.ascii_lowercase != low end)
  Client(
    cookies: client.cookies,
    inertia_version: client.inertia_version,
    default_headers: kept.append((name, value))
  )
end

fn prepare_inner(client: Client, req: Api.Request, inject_csrf?: Bool) : Api.Request
  var out = req
  for (hname, hval) in client.default_headers
    if out.header(hname) == None
      out = out.with_header(hname, hval)
    end
  end

  if not client.cookies.empty?
    jar_line = client.cookies.entries.map(fn(e) "#{e.0}=#{e.1}" end).join("; ")
    case out.header("Cookie")
      None: out = out.with_header("Cookie", jar_line)
      Some(existing): out = out.with_header("Cookie", "#{jar_line}; #{existing}")
    end
  end

  if client.inertia_version is Some(ver)
    if out.header("X-Inertia") == None
      out = out.with_header("X-Inertia", "true")
    end
    if not ver.empty? and out.header("X-Inertia-Version") == None
      out = out.with_header("X-Inertia-Version", ver)
    end
  end

  if inject_csrf? and not Csrf.safe_method?(out) and out.header(Csrf.header_name) == None
    if client.cookies.get(Csrf.cookie_name) is Some(xsrf)
      if not xsrf.empty?
        decoded = Api.percent_decode(xsrf, false) or xsrf
        out = out.with_header(Csrf.header_name, decoded)
      end
    end
  end
  out
end

## Attaches `client`'s cookies, default headers, Inertia headers (if enabled), and automatic
## `X-XSRF-TOKEN` (from the `XSRF-TOKEN` cookie on unsafe methods unless already set on `req`).
fn prepare(client: Client, req: Api.Request) : Api.Request
  prepare_inner(client, req, true)
end

## Like `prepare`, but omits automatic `X-XSRF-TOKEN` injection to test server-side CSRF rejection.
fn prepare_without_csrf(client: Client, req: Api.Request) : Api.Request
  prepare_inner(client, req, false)
end

fn clears_cookie?(attrs: List(Text), val: Text) : Bool
  for raw_attr in attrs
    attr = raw_attr.trim.ascii_lowercase
    if attr.starts_with?("max-age=")
      age_str = (attr.drop_prefix("max-age=") or "").trim
      case age_str.to_int
        Some(n): return true if n <= 0
        None: Unit
      end
    else if val.empty? and attr.starts_with?("expires=") and attr.contains?("1970")
      return true
    end
  end
  false
end

## Absorbs all `Set-Cookie` headers from `res` into `client`'s cookie jar, deleting cleared cookies.
fn absorb(client: Client, res: Api.Response) : Client
  var jar = client.cookies
  for line in res.headers_named("Set-Cookie")
    segs = line.split(";")
    first = (segs.first or "").trim
    continue if first.empty? or not first.contains?("=")
    kv = first.split("=")
    cname = (kv.first or "").trim
    continue if cname.empty?
    cval = kv.drop(1).join("=").trim
    if clears_cookie?(segs.drop(1), cval)
      jar = jar.remove(cname)
    else
      jar = jar.set(cname, cval)
    end
  end
  Client(
    cookies: jar,
    inertia_version: client.inertia_version,
    default_headers: client.default_headers
  )
end

## Looks up a cookie currently stored in `client`'s jar.
fn cookie(client: Client, name: Text) : Option(Text)
  client.cookies.get(name)
end

## Returns the `Location` target of a 3xx redirect response (or `None` when not a redirect).
fn redirect_location(res: Api.Response) : Option(Text)
  return None if res.status < 300 or res.status >= 400
  res.header("Location")
end

fn unescape_html_attr(escaped: Text) : Text
  escaped.replace("&quot;", "\"").replace("&#39;", "'").replace("&#x27;", "'").replace(
    "&#X27;",
    "'"
  ).replace("&apos;", "'").replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&")
end

fn extract_data_page(html: Text) : Option(Text)
  marker_dq = "data-page=\""
  parts_dq = html.split(marker_dq)
  if parts_dq.size >= 2
    after = parts_dq.drop(1).join(marker_dq)
    before_close = after.split("\"").first or ""
    return Some(unescape_html_attr(before_close))
  end

  marker_sq = "data-page='"
  parts_sq = html.split(marker_sq)
  if parts_sq.size >= 2
    after = parts_sq.drop(1).join(marker_sq)
    before_close = after.split("'").first or ""
    return Some(unescape_html_attr(before_close))
  end
  None
end

## Decodes an `InertiaPage` from either an `X-Inertia: true` JSON response or an initial HTML
## response's `data-page="..."` attribute.
fn inertia_page(res: Api.Response) : Result(InertiaPage, Text)
  ct = (res.header("Content-Type") or "").ascii_lowercase
  if res.header("X-Inertia") == Some("true") or ct.starts_with?("application/json")
    Json.decode[InertiaPage](res.body).map_error(fn(e) e.to_text end)
  else
    raw_json = try extract_data_page(res.text).or_fail(
      "response contains no Inertia data-page attribute"
    )
    Json.decode[InertiaPage](raw_json).map_error(fn(e) e.to_text end)
  end
end

test "WebTest client tracks Set-Cookie jars, injects CSRF, clears on logout, and decodes Inertia HTML & JSON"
  login_res = Api.redirect_see_other("/").add_header(
    "Set-Cookie",
    "arvo_session=sealed_123; Path=/; Max-Age=3600; HttpOnly; Secure"
  ).add_header("Set-Cookie", "XSRF-TOKEN=csrf_456; Path=/; SameSite=Lax; Secure")
  assert redirect_location(login_res) == Some("/")

  c1 = absorb(new(), login_res)
  assert cookie(c1, "arvo_session") == Some("sealed_123")
  assert cookie(c1, "XSRF-TOKEN") == Some("csrf_456")

  post_req = prepare(c1, Api.post("/bills"))
  assert post_req.header("X-XSRF-TOKEN") == Some("csrf_456")
  assert (post_req.header("Cookie") or "").contains?("arvo_session=sealed_123")

  no_csrf_req = prepare_without_csrf(c1, Api.post("/bills"))
  assert no_csrf_req.header("X-XSRF-TOKEN") == None

  c_inertia = with_inertia(with_header(c1, "Host", "budget.local"), "v_sha1")
  get_xhr = prepare(c_inertia, Api.get("/bills"))
  assert get_xhr.header("X-Inertia") == Some("true")
  assert get_xhr.header("X-Inertia-Version") == Some("v_sha1")
  assert get_xhr.header("Host") == Some("budget.local")
  assert get_xhr.header("X-XSRF-TOKEN") == None

  c_html = without_inertia(c_inertia)
  assert prepare(c_html, Api.get("/bills")).header("X-Inertia") == None

  html_shell = """
    <!doctype html>
    <html><body>
      <div id="app" data-page="{&quot;component&quot;:&quot;Home/Index&quot;,&quot;props&quot;:{&quot;note&quot;:&quot;a &amp;lt; b &amp; &#39;ok&#39;&quot;},&quot;url&quot;:&quot;/&quot;,&quot;version&quot;:&quot;v1&quot;}"></div>
    </body></html>
    """
  case inertia_page(Api.html_response(200, html_shell))
    Error(_): stop "unexpected html data-page error"
    Ok(page):
      assert page.component == "Home/Index"
      assert page.url == "/"
      assert page.version == "v1"
      assert (page.props.get("note") or Json.Null) == Json.String("a &lt; b & 'ok'")
  end

  json_res = Api.encoded_json_response(
    200,
    Json.Object({
      "component": Json.String("Bills/Show"),
      "props": Json.Object({"id": Json.int(42)}),
      "url": Json.String("/bills/42"),
      "version": Json.String("v1")
    })
  ).with_header("X-Inertia", "true")
  case inertia_page(json_res)
    Error(_): stop "unexpected json page error"
    Ok(page):
      assert page.component == "Bills/Show"
      assert page.url == "/bills/42"
  end

  logout_res = Api.redirect_see_other("/login").add_header(
    "Set-Cookie",
    "arvo_session=; Path=/; Max-Age=0; HttpOnly"
  ).add_header("Set-Cookie", "XSRF-TOKEN=; Path=/; Max-Age=0")
  c_after = absorb(c1, logout_res)
  assert cookie(c_after, "arvo_session") == None
  assert cookie(c_after, "XSRF-TOKEN") == None
end
