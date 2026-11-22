## Inertia v2 server protocol: `X-Inertia` JSON responses, XSS-hardened initial HTML shell,
## Vite dev HMR and production `manifest.json` assets, partial reloads, and `303`/`409` redirects.
module Arvo.Inertia
expose InertiaConfig(..),
dev_config,
request?,
escape_html,
page_url,
version_from_manifest,
config_from_manifest,
version_mismatch?,
check_version,
render,
render_with_status,
redirect,
location,
validation_error
use Api
use Arvo.Flash
use Crypto
use Json

## Server configuration for rendering Inertia HTML shells and versioned responses.
struct InertiaConfig
  app_title: Text
  version: Text
  vite_dev_url: Option(Text)
  entry_js: Text
  entry_css: List(Text)
  head_extra: Text
end

## Builds a development `InertiaConfig` pointing at a running Vite dev server (`http://127.0.0.1:5173`).
fn dev_config(app_title: Text, vite_dev_url: Text, entry_js: Text, head_extra: Text) : InertiaConfig
  InertiaConfig(
    app_title: app_title,
    version: "dev",
    vite_dev_url: Some(vite_dev_url),
    entry_js: entry_js,
    entry_css: [],
    head_extra: head_extra
  )
end

## Whether `req` is an Inertia XHR navigation (`X-Inertia: true`).
fn request?(req: Api.Request) : Bool
  examples
    request?(Api.get("/").with_header("X-Inertia", "true"))
    not request?(Api.get("/"))
    not request?(Api.get("/").with_header("X-Inertia", "false"))
  end

  req.header("X-Inertia") == Some("true")
end

## Escapes `&`, `"`, `'`, `<`, and `>` so arbitrary JSON or text is safe inside HTML attributes and elements.
fn escape_html(raw: Text) : Text
  examples
    escape_html("a&b\"c'd<e>f") == "a&amp;b&quot;c&#39;d&lt;e&gt;f"
    escape_html("plain") == "plain"
  end

  promise "escaped HTML contains no raw <, >, \", or '"
    not result.contains?("<") and
      not result.contains?(">") and
      not result.contains?("\"") and
      not result.contains?("'")
  end

  raw.replace("&", "&amp;").replace("\"", "&quot;").replace("'", "&#39;").replace("<", "&lt;")
    .replace(">", "&gt;")
end

fn hex_upper_char(n: Int) : Text
  ("0123456789ABCDEF".chars.get(n) or 'F').to_text
end

fn unreserved_byte?(b: Int) : Bool
  (b >= 65 and b <= 90) or (b >= 97 and b <= 122) or (b >= 48 and b <= 57) or
    b == 45 or
    b == 46 or
    b == 95 or
    b == 126
end

fn encode_query_component(s: Text) : Text
  bs = s.bytes
  var parts = []
  var i = 0
  while i < bs.size
    b = (bs.get(i) or 0).to_int
    if unreserved_byte?(b)
      ch = b.to_char or '_'
      parts = parts.append(ch.to_text)
    else
      hi = hex_upper_char(b.floor_div(16))
      lo = hex_upper_char(b.floor_mod(16))
      parts = parts.append("%#{hi}#{lo}")
    end
    i += 1
  end
  parts.join("")
end

## Reconstructs the request path including any query string (`"/bills?page=2"`).
fn page_url(req: Api.Request) : Text
  examples
    page_url(Api.get("/bills")) == "/bills"
    page_url(Api.get("/bills?page=2&q=a+b")) == "/bills?page=2&q=a%20b"
  end

  return req.path if req.query.empty?
  qs = req.query.map(fn(p) "#{encode_query_component(p.0)}=#{encode_query_component(p.1)}" end).join(
    "&"
  )
  "#{req.path}?#{qs}"
end

## Computes the deterministic SHA-256 hex asset version of a Vite `manifest.json`.
fn version_from_manifest(manifest_bytes: Bytes) : Text
  Crypto.sha256(manifest_bytes).to_hex
end

fn leading_slash(path: Text) : Text
  if path.starts_with?("/"): path else: "/#{path}"
end

fn extract_css_list(entry_obj: Map(Text, Json)) : Result(List(Text), Text)
  case entry_obj.get("css")
    None: Ok([])
    Some(css_val):
      case css_val
        Json.Null: Ok([])
        Json.Array(items):
          var out = []
          for item in items
            case item
              Json.String(css_path):
                return Error("manifest css entry is empty") if css_path.empty?
                out = out.append(leading_slash(css_path))
              Json.Null | Json.Boolean(_) | Json.Number(_) | Json.Array(_) | Json.Object(_): return Error(
                "manifest css array must contain strings"
              )
            end
          end
          Ok(out)
        Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Object(_): Error(
          "manifest css field must be an array"
        )
      end
  end
end

## Parses a Vite production `manifest.json`, resolving hashed `file` and `css` paths for `entry_key`.
fn config_from_manifest(app_title: Text, manifest_bytes: Bytes, entry_key: Text, head_extra: Text) : Result(
  InertiaConfig,
  Text
)
  raw_text = try manifest_bytes.decode_utf8.or_fail("manifest is not valid UTF-8")
  root_json = try Json.parse(raw_text).map_error(fn(e) "invalid manifest JSON: #{e}" end)
  case root_json
    Json.Object(entries):
      case entries.get(entry_key)
        None: Error("manifest missing entry '#{entry_key}'")
        Some(entry_val):
          case entry_val
            Json.Object(entry_obj):
              entry_file = case entry_obj.get("file")
                Some(file_val):
                  case file_val
                    Json.String(f):
                      return Error("manifest entry file is empty") if f.empty?
                      leading_slash(f)
                    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.Array(_) | Json.Object(_): return Error(
                      "manifest entry '#{entry_key}' missing string 'file'"
                    )
                  end
                None: return Error("manifest entry '#{entry_key}' missing string 'file'")
              end
              css_files = try extract_css_list(entry_obj)
              Ok(
                InertiaConfig(
                  app_title: app_title,
                  version: version_from_manifest(manifest_bytes),
                  vite_dev_url: None,
                  entry_js: entry_file,
                  entry_css: css_files,
                  head_extra: head_extra
                )
              )
            Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): Error(
              "manifest entry '#{entry_key}' must be an object"
            )
          end
      end
    Json.Null | Json.Boolean(_) | Json.Number(_) | Json.String(_) | Json.Array(_): Error(
      "manifest root must be a JSON object"
    )
  end
end

fn sanitize_header_value(raw: Text, fallback: Text) : Text
  if raw.empty? or raw.contains?("\r") or raw.contains?("\n") or raw.contains?("\u{0}")
    fallback
  else
    raw
  end
end

## Builds a `409 Conflict` Inertia external/full-reload response with `X-Inertia-Location`.
fn location(url: Text) : Api.Response
  safe_url = sanitize_header_value(url, "/")
  Api.response(409, [("X-Inertia-Location", safe_url)], "".bytes)
end

## Builds a `303 See Other` redirect response (required by Inertia after `POST`/`PUT`/`PATCH`/`DELETE`).
fn redirect(target: Text) : Api.Response
  safe_target = sanitize_header_value(target, "/")
  Api.redirect_see_other(safe_target)
end

## Whether an Inertia `GET` request carries a stale `X-Inertia-Version` header.
fn version_mismatch?(req: Api.Request, cfg: InertiaConfig) : Bool
  return false if req.method.ascii_uppercase != "GET" or not request?(req)
  case req.header("X-Inertia-Version")
    None: false
    Some(client_version): not Crypto.constant_time_eq?(client_version.bytes, cfg.version.bytes)
  end
end

## Returns `Some(409 Conflict + X-Inertia-Location)` when `version_mismatch?(req, cfg)` is `true`.
fn check_version(req: Api.Request, cfg: InertiaConfig) : Option(Api.Response)
  if version_mismatch?(req, cfg)
    Some(location(page_url(req)))
  else
    None
  end
end

fn parse_csv_set(raw: Text) : Set(Text)
  raw.split(",").map(fn(s) s.trim end).filter(fn(s) not s.empty? end).to_set
end

fn filter_partial_props(req: Api.Request, component: Text, merged: Map(Text, Json)) : Map(
  Text,
  Json
)
  return merged if req.header("X-Inertia-Partial-Component") != Some(component)

  after_only = case req.header("X-Inertia-Partial-Data")
    None: merged
    Some(raw_only):
      only_keys = parse_csv_set(raw_only)
      if only_keys.empty?
        merged
      else
        var picked = {}
        for (k, v) in merged
          if only_keys.has?(k) or k == "errors"
            picked = picked.set(k, v)
          end
        end
        picked
      end
  end

  case req.header("X-Inertia-Partial-Except")
    None: after_only
    Some(raw_except):
      except_keys = parse_csv_set(raw_except)
      if except_keys.empty?
        after_only
      else
        var kept = {}
        for (k, v) in after_only
          if not except_keys.has?(k)
            kept = kept.set(k, v)
          end
        end
        kept
      end
  end
end

fn render_asset_tags(cfg: InertiaConfig) : Text
  case cfg.vite_dev_url
    Some(dev_raw):
      base = escape_html(dev_raw.drop_suffix("/") or dev_raw)
      entry = escape_html(cfg.entry_js.drop_prefix("/") or cfg.entry_js)
      [
        "<script type=\"module\">",
        "import RefreshRuntime from \"#{base}/@react-refresh\"",
        "RefreshRuntime.injectIntoGlobalHook(window)",
        "window.$RefreshReg$ = () => {}",
        "window.$RefreshSig$ = () => (type) => type",
        "window.__vite_plugin_react_preamble_installed__ = true",
        "</script>",
        "<script type=\"module\" src=\"#{base}/@vite/client\"></script>",
        "<script type=\"module\" src=\"#{base}/#{entry}\"></script>"
      ].join("\n  ")
    None:
      css_tags = cfg.entry_css.map(
        fn(href) "<link rel=\"stylesheet\" href=\"#{escape_html(href)}\">" end
      )
      js_tag = "<script type=\"module\" src=\"#{escape_html(cfg.entry_js)}\"></script>"
      css_tags.append(js_tag).join("\n  ")
  end
end

fn render_html_shell(cfg: InertiaConfig, page_json_text: Text) : Text
  escaped_title = escape_html(cfg.app_title)
  escaped_page = escape_html(page_json_text)
  assets = render_asset_tags(cfg)
  extra = if cfg.head_extra.trim.empty?: "" else: "\n  #{cfg.head_extra}"
  [
    "<!doctype html>",
    "<html lang=\"en\">",
    "<head>",
    "  <meta charset=\"utf-8\">",
    "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">",
    "  <title>#{escaped_title}</title>#{extra}",
    "  #{assets}",
    "</head>",
    "<body>",
    "  <div id=\"app\" data-page=\"#{escaped_page}\"></div>",
    "</body>",
    "</html>"
  ].join("\n")
end

## Renders an Inertia v2 response with explicit HTTP `status` (JSON for `X-Inertia: true`, HTML shell otherwise).
fn render_with_status(
  req: Api.Request,
  cfg: InertiaConfig,
  status: Int,
  component: Text,
  props: Map(Text, Json),
  shared: Map(Text, Json)
) : Api.Response
  var merged = shared
  for (k, v) in props
    merged = merged.set(k, v)
  end
  final_props = filter_partial_props(req, component, merged)
  page_obj = Json.Object({
    "component": Json.String(component),
    "props": Json.Object(final_props),
    "url": Json.String(page_url(req)),
    "version": Json.String(cfg.version)
  })
  if request?(req)
    Api.encoded_json_response(status, page_obj).with_header(
      "Content-Type",
      "application/json; charset=utf-8"
    ).with_header("X-Inertia", "true").with_header("Vary", "X-Inertia")
  else
    html = render_html_shell(cfg, page_obj.to_text)
    Api.html_response(status, html).with_header("Vary", "X-Inertia")
  end
end

## Renders a `200 OK` Inertia v2 page response.
fn render(
  req: Api.Request,
  cfg: InertiaConfig,
  component: Text,
  props: Map(Text, Json),
  shared: Map(Text, Json)
) : Api.Response
  render_with_status(req, cfg, 200, component, props, shared)
end

fn safe_local_path?(p: Text) : Bool
  return false if not p.starts_with?("/") or p.starts_with?("//")
  return false if p.contains?("\\") or
    p.contains?("\r") or
    p.contains?("\n") or
    p.contains?("\u{0}")
  true
end

fn extract_same_origin_referer(req: Api.Request, fallback_url: Text) : Text
  clean_fallback = if safe_local_path?(fallback_url): fallback_url else: "/"
  raw_ref = case req.header("Referer")
    Some(r): r.trim
    None: return clean_fallback
  end
  return clean_fallback if raw_ref.empty? or
    raw_ref.contains?("\\") or
    raw_ref.contains?("\r") or
    raw_ref.contains?("\n") or
    raw_ref.contains?("\u{0}")

  no_frag = raw_ref.split("#").first or raw_ref
  if safe_local_path?(no_frag)
    return no_frag
  end

  after_scheme = case no_frag.drop_prefix("https://")
    Some(rest): rest
    None:
      case no_frag.drop_prefix("http://")
        Some(rest): rest
        None: return clean_fallback
      end
  end

  req_host = case req.header("Host")
    Some(h): h.trim.ascii_lowercase
    None: return clean_fallback
  end
  return clean_fallback if req_host.empty?

  slash_parts = after_scheme.split("/")
  ref_host = (slash_parts.first or "").trim.ascii_lowercase
  return clean_fallback if ref_host != req_host

  rel_path = if slash_parts.size <= 1
    "/"
  else
    "/#{slash_parts.drop(1).join("/")}"
  end
  if safe_local_path?(rel_path): rel_path else: clean_fallback
end

## Seals validation `errors` into `arvo_flash` and returns `303 See Other` back to a verified
## same-origin `Referer` path (or `fallback_url`), blocking external open redirects.
fn validation_error(
  req: Api.Request,
  rng: SecureRandom,
  key: Bytes,
  fallback_url: Text,
  errors: Map(Text, List(Text)),
  secure?: Bool
) : Api.Response
  target = extract_same_origin_referer(req, fallback_url)
  Flash.attach(redirect(target), rng, key, Flash.with_errors(errors), secure?)
end

test "Vite manifest config, HTML shell escaping, dev HMR, partial reloads, and open-redirect guard"
  manifest = """
    {
      "src/main.tsx": {
        "file": "assets/main-a1b2c3d4.js",
        "css": ["assets/main-9f8e7d6c.css"],
        "isEntry": true
      }
    }
    """
    .bytes

  cfg = case config_from_manifest(
    "Arvo <App>",
    manifest,
    "src/main.tsx",
    "<meta name=\"theme-color\" content=\"#111\">"
  )
    Ok(c): c
    Error(_): stop "unexpected manifest error"
  end
  assert cfg.entry_js == "/assets/main-a1b2c3d4.js"
  assert cfg.entry_css == ["/assets/main-9f8e7d6c.css"]
  assert cfg.version == version_from_manifest(manifest)

  html_res = render(
    Api.get("/bills?filter=open"),
    cfg,
    "Bills/Index",
    {"xss": Json.String("</script><script>alert(\"x\")</script>"), "count": Json.int(3)},
    {"auth": Json.String("ada"), "count": Json.int(0)}
  )
  assert html_res.status == 200
  assert html_res.header("Vary") == Some("X-Inertia")
  body_html = html_res.text
  assert body_html.contains?("<title>Arvo &lt;App&gt;</title>")
  assert body_html.contains?("<link rel=\"stylesheet\" href=\"/assets/main-9f8e7d6c.css\">")
  assert body_html.contains?("<script type=\"module\" src=\"/assets/main-a1b2c3d4.js\"></script>")
  assert not body_html.contains?("<script>alert")
  assert body_html.contains?("&lt;/script&gt;&lt;script&gt;alert")

  dev_cfg = dev_config("Dev", "http://127.0.0.1:5173/", "src/main.tsx", "")
  dev_html = render(Api.get("/"), dev_cfg, "Home/Index", {}, {}).text
  assert dev_html.contains?("http://127.0.0.1:5173/@vite/client")
  assert dev_html.contains?("http://127.0.0.1:5173/@react-refresh")
  assert dev_html.contains?("http://127.0.0.1:5173/src/main.tsx")

  xhr_req = Api.get("/bills?page=2").with_header("X-Inertia", "true").with_header(
    "X-Inertia-Version",
    cfg.version
  ).with_header("X-Inertia-Partial-Component", "Bills/Index").with_header(
    "X-Inertia-Partial-Data",
    "bills"
  )
  assert not version_mismatch?(xhr_req, cfg)
  assert check_version(xhr_req, cfg) == None
  xhr_res = render(
    xhr_req,
    cfg,
    "Bills/Index",
    {"bills": Json.Array([Json.String("Rent")]), "summary": Json.String("heavy")},
    {"errors": Json.Object({"name": Json.String("required")}), "auth": Json.String("ada")}
  )
  assert xhr_res.status == 200
  assert xhr_res.header("X-Inertia") == Some("true")
  assert xhr_res.header("Vary") == Some("X-Inertia")
  case xhr_res.json
    Error(_): assert false
    Ok(doc):
      assert doc.text_at("component") == Some("Bills/Index")
      assert doc.text_at("url") == Some("/bills?page=2")
      assert doc.text_at("version") == Some(cfg.version)
      assert doc.list_at("props.bills") != None
      assert doc.text_at("props.errors.name") == Some("required")
      assert doc.path("props.summary") == None
      assert doc.path("props.auth") == None
  end

  except_req = xhr_req.with_header("X-Inertia-Partial-Except", "errors")
  except_res = render(
    except_req,
    cfg,
    "Bills/Index",
    {"bills": Json.Array([])},
    {"errors": Json.Object({"name": Json.String("required")})}
  )
  case except_res.json
    Error(_): assert false
    Ok(doc): assert doc.path("props.errors") == None
  end

  stale_req = Api.get("/bills?tab=all").with_header("X-Inertia", "true").with_header(
    "X-Inertia-Version",
    "stale-sha"
  )
  assert version_mismatch?(stale_req, cfg)
  case check_version(stale_req, cfg)
    None: assert false
    Some(conflict):
      assert conflict.status == 409
      assert conflict.header("X-Inertia-Location") == Some("/bills?tab=all")
  end

  rng = SecureRandom.seeded(9)
  key = rng.bytes(32)
  evil_ref = Api.post("/bills").with_header("Host", "budget.local").with_header(
    "Referer",
    "https://evil.example/steal"
  )
  v_evil = validation_error(evil_ref, rng, key, "/bills/new", {"name": ["blank"]}, true)
  assert v_evil.status == 303
  assert v_evil.header("Location") == Some("/bills/new")

  proto_rel = Api.post("/bills").with_header("Referer", "//evil.example/steal")
  assert validation_error(proto_rel, rng, key, "/bills/new", {"name": ["blank"]}, true).header(
    "Location"
  ) ==
    Some("/bills/new")

  same_orig = Api.post("/bills").with_header("Host", "budget.local:8080").with_header(
    "Referer",
    "https://budget.local:8080/bills/edit?id=5#top"
  )
  assert validation_error(same_orig, rng, key, "/bills", {"name": ["blank"]}, true).header(
    "Location"
  ) ==
    Some("/bills/edit?id=5")
end
