## AEAD-sealed short-lived flash cookie (`arvo_flash`) carrying one-hop notices, alerts,
## and validation error bags across `303 See Other` redirects.
module Arvo.Flash
expose FlashData(..),
cookie_name,
empty,
empty?,
with_notice,
with_alert,
with_errors,
attach,
take,
clear_if_present
use Api
use Arvo.Cookie
use Crypto
use Json

cookie_name = "arvo_flash"
flash_aad = "arvo:flash:arvo_flash".bytes
flash_max_age = 120

## One-hop flash payload serialized as authenticated JSON inside `arvo_flash`.
struct FlashData
  notice: Option(Text)
  alert: Option(Text)
  errors: Map(Text, List(Text))
end

## An empty flash payload with no notice, alert, or validation errors.
fn empty() : FlashData
  FlashData(notice: None, alert: None, errors: {})
end

## Whether `data` carries no notice, no alert, and no validation errors.
fn empty?(data: FlashData) : Bool
  examples
    empty?(empty())
    not empty?(with_notice("Saved"))
    not empty?(with_alert("Denied"))
    not empty?(with_errors({"email": ["required"]}))
  end

  data.notice == None and data.alert == None and data.errors.empty?
end

## Builds flash data carrying a success/info `notice`.
fn with_notice(msg: Text) : FlashData
  FlashData(notice: Some(msg), alert: None, errors: {})
end

## Builds flash data carrying an error/warning `alert`.
fn with_alert(msg: Text) : FlashData
  FlashData(notice: None, alert: Some(msg), errors: {})
end

## Builds flash data carrying field validation `errors`.
fn with_errors(errs: Map(Text, List(Text))) : FlashData
  FlashData(notice: None, alert: None, errors: errs)
end

## Seals `data` with AES-256-GCM under `key` (32 bytes) and attaches `arvo_flash` (`Max-Age=120`).
fn attach(res: Api.Response, rng: SecureRandom, key: Bytes, data: FlashData, secure?: Bool) : Api
  .Response
  nonce = rng.bytes(12)
  plaintext = Json.encode(data).bytes
  case Crypto.seal(key, nonce, plaintext, flash_aad)
    Error(_): res
    Ok(ciphertext):
      encoded = Bytes.join([nonce, ciphertext]).to_base64url
      opts = Cookie.CookieOptions(
        path: "/",
        max_age: Some(flash_max_age),
        http_only?: true,
        secure?: secure?,
        same_site: Cookie.Lax
      )
      Cookie.set(res, cookie_name, encoded, opts)
  end
end

fn open_flash_cookie(raw_cookie: Text, key: Bytes) : Option(FlashData)
  raw_bytes = try Bytes.from_base64url(raw_cookie)
  return None if raw_bytes.size < 28 or key.size != 32
  nonce = raw_bytes.slice(0, 12)
  ciphertext = raw_bytes.slice(12, raw_bytes.size)
  case Crypto.open(key, nonce, ciphertext, flash_aad)
    Error(_): None
    Ok(plaintext):
      case Json.decode_strict[FlashData](plaintext)
        Ok(data): Some(data)
        Error(_): None
      end
  end
end

## Reads and authenticates `arvo_flash` from `req`, returning `(data, had_cookie?)`.
## Fails closed to `(empty(), true)` if a malformed or tampered cookie was present so callers clear it.
fn take(req: Api.Request, key: Bytes) : (FlashData, Bool)
  case Cookie.get(req, cookie_name)
    None: (empty(), false)
    Some(raw):
      case open_flash_cookie(raw, key)
        Some(data): (data, true)
        None: (empty(), true)
      end
  end
end

## Appends a cookie-clearing `Set-Cookie` for `arvo_flash` when `had_cookie?` is `true`.
fn clear_if_present(res: Api.Response, had_cookie?: Bool, secure?: Bool) : Api.Response
  if had_cookie?
    Cookie.clear(res, cookie_name, "/", secure?)
  else
    res
  end
end

test "flash round-trips across 303 redirect and fails closed on tampering"
  rng = SecureRandom.seeded(101)
  key = rng.bytes(32)
  other_key = rng.bytes(32)

  data = FlashData(
    notice: Some("Transaction imported"),
    alert: None,
    errors: {"amount": ["must be greater than 0"]}
  )
  redir = attach(Api.redirect_see_other("/"), rng, key, data, true)
  set_hdr = redir.header("Set-Cookie") or ""
  assert set_hdr.starts_with?("arvo_flash=")
  assert set_hdr.contains?("Max-Age=120")
  assert set_hdr.contains?("HttpOnly")
  assert set_hdr.contains?("SameSite=Lax")
  assert set_hdr.contains?("Secure")

  cookie_pair = (set_hdr.split(";").first or "").trim
  next_req = Api.get("/").with_header("Cookie", cookie_pair)
  (loaded, had?) = take(next_req, key)
  assert had?
  assert loaded == data

  cleared = clear_if_present(Api.html_response(200, "<p>ok</p>"), had?, true)
  assert (cleared.header("Set-Cookie") or "").contains?("arvo_flash=; Path=/; Max-Age=0")

  (missing_data, missing_had?) = take(Api.get("/"), key)
  assert not missing_had?
  assert empty?(missing_data)
  unchanged = clear_if_present(Api.html_response(200, "<p>ok</p>"), missing_had?, true)
  assert unchanged.header("Set-Cookie") == None

  (wrong_key_data, wrong_key_had?) = take(next_req, other_key)
  assert wrong_key_had?
  assert empty?(wrong_key_data)

  tampered_req = Api.get("/").with_header("Cookie", "arvo_flash=not_valid_ciphertext")
  (tampered_data, tampered_had?) = take(tampered_req, key)
  assert tampered_had?
  assert empty?(tampered_data)
end
