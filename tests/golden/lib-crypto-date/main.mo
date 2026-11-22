module Main
use Crypto
use Date
use Json

test "Crypto, SecureRandom, Bytes hex/base64url, and Date work together"
  rng = SecureRandom.seeded(42)
  key = rng.bytes(32)
  nonce = rng.bytes(12)
  assert Crypto.seal(key, nonce, "arvo".bytes, "v1".bytes) is Ok(ct)
  assert Crypto.open(key, nonce, ct, "v1".bytes) == Ok("arvo".bytes)
  assert Crypto.open(key, nonce, ct, "v2".bytes) == Error(Crypto.DecryptionFailed)
  assert Crypto.sha256("abc".bytes).to_hex ==
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
  assert Crypto.constant_time_eq?(key, key)
  assert Bytes.from_hex(key.to_hex) == Some(key)
  assert Bytes.from_base64url(key.to_base64url) == Some(key)
  assert Date.parse_iso("2026-01-31") is Some(d)
  assert d.add_months(1) == Date.from_ymd(2026, 2, 28)
  assert d.short_label == "Jan 31, 2026"
  assert Json.encode(d) == "\"2026-01-31\""
  assert Json.decode[Date]("\"2026-01-31\"") == Ok(d)
end
