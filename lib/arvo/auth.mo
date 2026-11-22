## App-level authentication helpers: email normalization and constant-time allowlist checks,
## scrypt password hashing/verification, dual-cookie login/logout responses, and CSRF guards.
module Arvo.Auth
expose normalize_email,
email_allowed?,
hash_password,
verify_password,
login_response,
logout_response,
require_csrf
use Api
use Arvo.Csrf
use Arvo.Session
use Crypto

## Trims leading/trailing whitespace and lowercases ASCII letters in `email`.
fn normalize_email(email: Text) : Text
  examples
    normalize_email("  Ada.Lovelace@Example.COM ") == "ada.lovelace@example.com"
  end

  promise "normalize_email is idempotent and trimmed"
    normalize_email(result) == result and result.trim == result
  end

  email.trim.ascii_lowercase
end

## Checks whether `email` matches an entry in `allowlist` (normalized, constant-time per entry).
fn email_allowed?(email: Text, allowlist: List(Text)) : Bool
  examples
    email_allowed?("  ROBERT@example.com ", ["robert@example.com", "wife@example.com"])
    not email_allowed?("intruder@example.com", ["robert@example.com", "wife@example.com"])
    not email_allowed?("", ["robert@example.com"])
  end

  candidate = normalize_email(email)
  return false if candidate.empty?
  cand_bytes = candidate.bytes
  var matched = false
  for entry in allowlist
    norm_entry = normalize_email(entry)
    if not norm_entry.empty? and Crypto.constant_time_eq?(cand_bytes, norm_entry.bytes)
      matched = true
    end
  end
  matched
end

## Draws a fresh 16-byte CSPRNG salt from `rng` and hashes `password` with scrypt (`Crypto.hash_password`).
fn hash_password(rng: SecureRandom, password: Text) : Result(Text, Crypto.CryptoError)
  salt = rng.bytes(16)
  Crypto.hash_password(password, salt)
end

## Verifies `password` against a PHC `$scrypt$...` hash string in constant time.
fn verify_password(password: Text, encoded: Text) : Bool
  Crypto.verify_password(password, encoded)
end

## Attaches BOTH the `HttpOnly` encrypted session cookie AND the readable `XSRF-TOKEN` cookie to `res`.
fn login_response(
  res: Api.Response,
  session: Session.Session,
  cookie_val: Text,
  cfg: Session.SessionConfig
) : Api.Response
  with_sess = Session.attach_cookie(res, cookie_val, cfg)
  Csrf.attach_cookie(with_sess, session, cfg.secure?)
end

## Clears BOTH the session cookie and the `XSRF-TOKEN` cookie on `res`.
fn logout_response(res: Api.Response, cfg: Session.SessionConfig) : Api.Response
  cleared_sess = Session.clear_cookie(res, cfg)
  Csrf.clear_cookie(cleared_sess, cfg.secure?)
end

## Returns `true` when `req` uses a safe read method (`GET`/`HEAD`/`OPTIONS`) or carries a valid `X-XSRF-TOKEN`.
fn require_csrf(req: Api.Request, session: Session.Session) : Bool
  Csrf.safe_method?(req) or Csrf.verify(req, session)
end

test "allowlist, password hashing, dual Set-Cookie login/logout, and require_csrf"
  allow = ["Robert@Household.Local", "Partner@Household.Local"]
  assert email_allowed?(" partner@household.local ", allow)
  assert not email_allowed?("other@household.local", allow)

  rng = SecureRandom.seeded(55)
  encoded = case hash_password(rng, "s3cure-passphrase!")
    Ok(h): h
    Error(_): stop "hash_password failed"
  end
  assert verify_password("s3cure-passphrase!", encoded)
  assert not verify_password("wrong-passphrase", encoded)

  exp = Time.parse("2026-12-01T00:00:00Z") or stop "time"
  sess = Session.Session(
    id: "sess_abc",
    member_id: "mem_1",
    csrf_token: "csrf_tok_xyz",
    expires_at: exp
  )
  cfg = Session.default_config(true)

  login_res = login_response(Api.redirect_see_other("/"), sess, "sealed_val", cfg)
  assert login_res.status == 303
  set_cookies = login_res.headers_named("Set-Cookie")
  assert set_cookies.size == 2
  assert (set_cookies.get(0) or "").starts_with?("arvo_session=sealed_val;")
  assert (set_cookies.get(0) or "").contains?("HttpOnly")
  assert (set_cookies.get(1) or "").starts_with?("XSRF-TOKEN=csrf_tok_xyz;")
  assert not (set_cookies.get(1) or "").contains?("HttpOnly")

  logout_res = logout_response(Api.redirect_see_other("/login"), cfg)
  clear_cookies = logout_res.headers_named("Set-Cookie")
  assert clear_cookies.size == 2
  assert (clear_cookies.get(0) or "").contains?("arvo_session=; Path=/; Max-Age=0")
  assert (clear_cookies.get(1) or "").contains?("XSRF-TOKEN=; Path=/; Max-Age=0")

  assert require_csrf(Api.get("/budget"), sess)
  assert not require_csrf(Api.post("/budget"), sess)
  assert require_csrf(Api.post("/budget").with_header("X-XSRF-TOKEN", "csrf_tok_xyz"), sess)
end
