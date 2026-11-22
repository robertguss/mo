module Main
use Api
use Crypto
use Date
use Json

struct TokenResponse
  token: Text
  issued_on: Date
  digest: Text
end

api
  get "/token" issue_token
end

fn issue_token(rng: SecureRandom, clock: Clock) : TokenResponse
  raw = rng.bytes(16)
  today = Date.from_time_utc(clock.now)
  TokenResponse(token: raw.to_base64url, issued_on: today, digest: Crypto.sha256(raw).to_hex)
end

fn main(console: Console, rng: SecureRandom)
  key = rng.bytes(32)
  nonce = rng.bytes(12)
  case Crypto.seal(key, nonce, "arvo".bytes, "v1".bytes)
    Ok(ct):
      case Crypto.open(key, nonce, ct, "v1".bytes)
        Ok(pt): console.print(pt.decode_utf8 or "")
        Error(_): console.print("open_err")
      end
    Error(_): console.print("seal_err")
  end
end

test "ApiTest.send accepts SecureRandom.seeded and Date encodes as ISO string"
  t0 = Time.parse("2026-11-21T12:00:00Z")
  case t0
    Some(now):
      r1 = ApiTest.send(Main, Api.get("/token"), SecureRandom.seeded(42), Clock.fixed(now))
      r2 = ApiTest.send(Main, Api.get("/token"), SecureRandom.seeded(42), Clock.fixed(now))
      assert r1.status == 200
      assert r1.body == r2.body
      case Json.decode[TokenResponse](r1.body)
        Ok(res):
          assert res.issued_on == (Date.from_ymd(2026, 11, 21) or res.issued_on)
          assert Bytes.from_base64url(res.token) != None
          assert Bytes.from_hex(res.digest) != None
        Error(_): assert false
      end
    None: assert false
  end
end
