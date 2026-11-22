## Cryptographic primitives wrapping OpenSSL 3 `libcrypto`: SHA-256, HMAC-SHA256,
## constant-time byte comparison, AES-256-GCM authenticated encryption, and scrypt password
## hashing in self-describing PHC format.
module Crypto
expose CryptoError(..), seal, open, hash_password

enum CryptoError
  InvalidKeyLength(expected: Int, got: Int)
  InvalidNonceLength(expected: Int, got: Int)
  InvalidSaltLength(got: Int)
  DecryptionFailed
  EncodingFailed(reason: Text)
end

fn parse_error(raw: Text) : CryptoError
  if raw.starts_with?("invalid_key_length:")
    got = (raw.drop_prefix("invalid_key_length:") or "").to_int or 0
    InvalidKeyLength(expected: 32, got: got)
  else if raw.starts_with?("invalid_nonce_length:")
    got = (raw.drop_prefix("invalid_nonce_length:") or "").to_int or 0
    InvalidNonceLength(expected: 12, got: got)
  else if raw.starts_with?("invalid_salt_length:")
    got = (raw.drop_prefix("invalid_salt_length:") or "").to_int or 0
    InvalidSaltLength(got: got)
  else if raw == "decryption_failed"
    DecryptionFailed
  else if raw.starts_with?("encoding_failed:")
    EncodingFailed(reason: raw.drop_prefix("encoding_failed:") or raw)
  else
    EncodingFailed(reason: raw)
  end
end

## Encrypts and authenticates `plaintext` with AES-256-GCM using a 32-byte `key`, a 12-byte
## `nonce`, and associated data `aad`. Returns `ciphertext || 16-byte tag`.
fn seal(key: Bytes, nonce: Bytes, plaintext: Bytes, aad: Bytes) : Result(Bytes, CryptoError)
  examples
    seal("short".bytes, "0123456789ab".bytes, "hi".bytes, "".bytes) ==
      Error(InvalidKeyLength(expected: 32, got: 5))
  end

  promise "keys not 32 bytes long return InvalidKeyLength"
    key.size != 32 implies result == Error(InvalidKeyLength(expected: 32, got: key.size))
  end

  case Crypto.seal_raw(key, nonce, plaintext, aad)
    Ok(ct): Ok(ct)
    Error(raw): Error(parse_error(raw))
  end
end

## Verifies the 16-byte AES-256-GCM authentication tag and decrypts `ciphertext` under `key`,
## `nonce`, and `aad`. Returns `Error(DecryptionFailed)` on any tag mismatch or truncation without
## releasing plaintext.
fn open(key: Bytes, nonce: Bytes, ciphertext: Bytes, aad: Bytes) : Result(Bytes, CryptoError)
  examples
    open("short".bytes, "0123456789ab".bytes, "".bytes, "".bytes) ==
      Error(InvalidKeyLength(expected: 32, got: 5))
  end

  promise "keys not 32 bytes long return InvalidKeyLength"
    key.size != 32 implies result == Error(InvalidKeyLength(expected: 32, got: key.size))
  end

  case Crypto.open_raw(key, nonce, ciphertext, aad)
    Ok(pt): Ok(pt)
    Error(raw): Error(parse_error(raw))
  end
end

## Hashes `password` with scrypt (`N=2^15, r=8, p=1`) using an explicit `salt` (16 to 128 bytes)
## and returns a self-describing PHC string `$scrypt$ln=15,r=8,p=1$<salt>$<hash>`.
fn hash_password(password: Text, salt: Bytes) : Result(Text, CryptoError)
  examples
    hash_password("secret", "short".bytes) == Error(InvalidSaltLength(got: 5))
    hash_password("secret", "".bytes) == Error(InvalidSaltLength(got: 0))
  end

  case Crypto.hash_password_raw(password, salt)
    Ok(encoded): Ok(encoded)
    Error(raw): Error(parse_error(raw))
  end
end

fn must_hex(hex: Text) : Bytes
  case Bytes.from_hex(hex)
    Some(b): b
    None: "".bytes
  end
end

test "FIPS 180-4 SHA-256 official test vectors"
  assert Crypto.sha256("".bytes).to_hex ==
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
  assert Crypto.sha256("abc".bytes).to_hex ==
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
  assert Crypto.sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq".bytes).to_hex ==
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
end

test "RFC 4231 HMAC-SHA256 official test vectors"
  k1 = must_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b")
  assert Crypto.hmac_sha256(k1, "Hi There".bytes).to_hex ==
    "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
  assert Crypto.hmac_sha256("Jefe".bytes, "what do ya want for nothing?".bytes).to_hex ==
    "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"
  k3 = must_hex("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
  d3 = must_hex(
    "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
  )
  assert Crypto.hmac_sha256(k3, d3).to_hex ==
    "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"
  k6 = must_hex("aa".repeat(131))
  assert Crypto.hmac_sha256(k6, "Test Using Larger Than Block-Size Key - Hash Key First".bytes)
    .to_hex ==
    "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"
end

test "constant_time_eq? compares equal and unequal byte strings"
  assert Crypto.constant_time_eq?("".bytes, "".bytes)
  assert Crypto.constant_time_eq?("token-123".bytes, "token-123".bytes)
  assert not Crypto.constant_time_eq?("token-123".bytes, "token-124".bytes)
  assert not Crypto.constant_time_eq?("short".bytes, "shorter".bytes)
end

test "NIST SP 800-38D AES-256-GCM vectors and tamper rejection"
  zero32 = must_hex("00".repeat(32))
  zero12 = must_hex("00".repeat(12))
  zero16 = must_hex("00".repeat(16))

  case seal(zero32, zero12, "".bytes, "".bytes)
    Ok(ct):
      assert ct.to_hex == "530f8afbc74536b9a963b4f1c4cb738b"
      assert open(zero32, zero12, ct, "".bytes) == Ok("".bytes)
    Error(_): assert false
  end

  case seal(zero32, zero12, zero16, "".bytes)
    Ok(ct):
      assert ct.to_hex == "cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919"
      assert open(zero32, zero12, ct, "".bytes) == Ok(zero16)
    Error(_): assert false
  end

  key16 = must_hex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308")
  iv16 = must_hex("cafebabefacedbaddecaf888")
  pt16 = must_hex(
    "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39"
  )
  aad16 = must_hex("feedfacedeadbeeffeedfacedeadbeefabaddad2")
  expected16 = "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f66276fc6ece0f4e1768cddf8853bb2d551b"

  case seal(key16, iv16, pt16, aad16)
    Ok(ct):
      assert ct.to_hex == expected16
      assert open(key16, iv16, ct, aad16) == Ok(pt16)
      assert open(key16, iv16, ct, "tampered".bytes) == Error(DecryptionFailed)
      assert open(zero32, iv16, ct, aad16) == Error(DecryptionFailed)
      assert open(key16, zero12, ct, aad16) == Error(DecryptionFailed)
      bad_ct = must_hex("53#{expected16.drop_prefix("52") or ""}")
      assert open(key16, iv16, bad_ct, aad16) == Error(DecryptionFailed)
      bad_tag = must_hex("#{expected16.drop_suffix("1b") or ""}1a")
      assert open(key16, iv16, bad_tag, aad16) == Error(DecryptionFailed)
      assert open(key16, iv16, ct.slice(0, 15), aad16) == Error(DecryptionFailed)
      assert open(key16, iv16, "".bytes, aad16) == Error(DecryptionFailed)
    Error(_): assert false
  end

  assert seal(zero16, zero12, "".bytes, "".bytes) == Error(InvalidKeyLength(expected: 32, got: 16))
  assert seal(zero32, "short".bytes, "".bytes, "".bytes) ==
    Error(InvalidNonceLength(expected: 12, got: 5))
  assert open(zero16, zero12, zero16, "".bytes) == Error(InvalidKeyLength(expected: 32, got: 16))
  assert open(zero32, "short".bytes, zero16, "".bytes) ==
    Error(InvalidNonceLength(expected: 12, got: 5))
end

test "RFC 7914 scrypt vectors and hash_password round-trip"
  v1 = "$scrypt$ln=4,r=1,p=1$$d9ZXYjhleyA7GcpCwYoEl/FrSETjB0ro39/6P+3iFEL80Aad7QlI+DJqdToPyB8X6NPg+y4NNijPNeIMONGJBg"
  assert Crypto.verify_password("", v1)
  assert not Crypto.verify_password("x", v1)

  v2 = "$scrypt$ln=10,r=8,p=16$TmFDbA$/bq+HJ00cgB4VucZDQHp/nxq18vII3gw53N2Y0s3MWIurzDZLiKjiG/xCSedmDDaxyevuUqD7m2DYMvfoswGQA"
  assert Crypto.verify_password("password", v2)
  assert not Crypto.verify_password("Password", v2)

  v3 = "$scrypt$ln=14,r=8,p=1$U29kaXVtQ2hsb3JpZGU$cCO9yzr9c0hGHAbNgf046/2o+7qQT44+qbVD9lRdofLVQylVYT8Pz2LUlwUkKpr55h6F3A1lHkDfzwF7RVdYhw"
  assert Crypto.verify_password("pleaseletmein", v3)
  assert not Crypto.verify_password("pleaseletmeout", v3)

  assert not Crypto.verify_password("password", "not-a-phc-string")
  assert not Crypto.verify_password("password", "$scrypt$ln=20,r=8,p=1$AAAA$AAAA")
  assert not Crypto.verify_password("password", "$scrypt$ln=04,r=1,p=1$$AAAA")

  rng = SecureRandom.seeded(42)
  salt = rng.bytes(16)
  case hash_password("correct horse battery staple", salt)
    Ok(encoded):
      assert encoded.starts_with?("$scrypt$ln=15,r=8,p=1$")
      assert Crypto.verify_password("correct horse battery staple", encoded)
      assert not Crypto.verify_password("wrong password", encoded)
    Error(_): assert false
  end

  assert hash_password("secret", rng.bytes(15)) == Error(InvalidSaltLength(got: 15))
  assert hash_password("secret", rng.bytes(129)) == Error(InvalidSaltLength(got: 129))
end

test "RFC 4648 hex and unpadded base64url encodings"
  assert "".bytes.to_hex == ""
  assert "f".bytes.to_hex == "66"
  assert "foobar".bytes.to_hex == "666f6f626172"
  assert Bytes.from_hex("") == Some("".bytes)
  assert Bytes.from_hex("666F6f626172") == Some("foobar".bytes)
  assert Bytes.from_hex("abc") == None
  assert Bytes.from_hex("zzzz") == None

  assert "".bytes.to_base64url == ""
  assert "f".bytes.to_base64url == "Zg"
  assert "fo".bytes.to_base64url == "Zm8"
  assert "foo".bytes.to_base64url == "Zm9v"
  assert "foob".bytes.to_base64url == "Zm9vYg"
  assert "fooba".bytes.to_base64url == "Zm9vYmE"
  assert "foobar".bytes.to_base64url == "Zm9vYmFy"
  special = must_hex("fbff")
  assert special.to_base64url == "-_8"
  assert Bytes.from_base64url("-_8") == Some(special)
  assert Bytes.from_base64url("Zm9vYmFy") == Some("foobar".bytes)
  assert Bytes.from_base64url("Zg==") == None
  assert Bytes.from_base64url("a") == None
  assert Bytes.from_base64url("Zh") == None
  assert Bytes.from_base64url("+/8") == None
end

test "SecureRandom.seeded is deterministic per seed and advances across calls"
  r1 = SecureRandom.seeded(99)
  r2 = SecureRandom.seeded(99)
  r3 = SecureRandom.seeded(100)
  a = r1.bytes(32)
  b = r2.bytes(32)
  c = r1.bytes(32)
  d = r3.bytes(32)
  assert a.size == 32
  assert a == b
  assert a != c
  assert a != d
  assert r1.bytes(0).size == 0
  assert r1.bytes(256).size == 256
end
