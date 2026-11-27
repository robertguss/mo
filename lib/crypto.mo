## Cryptographic primitives wrapping pure-Rust `sha2`, `hmac`, `aes-gcm`, `scrypt`, and `ring`:
## SHA-256, HMAC-SHA1/256, constant-time byte comparison, AES-256-GCM AEAD, scrypt passwords,
## and Ed25519 / P-256 ECDSA / RSA PKCS#1 v1.5 signature verification.
module Crypto
expose CryptoError(..), SignatureAlg(..), seal, open, hash_password, verify_signature

enum CryptoError
  InvalidKeyLength(expected: Int, got: Int)
  InvalidNonceLength(expected: Int, got: Int)
  InvalidSaltLength(got: Int)
  DecryptionFailed
  EncodingFailed(reason: Text)
  UnsupportedAlgorithm(alg: Text)
end

## Supported asymmetric signature verification algorithms for WebAuthn Passkeys and OIDC JWKS.
enum SignatureAlg
  Ed25519
  Es256Asn1
  Es256Fixed
  Rs256
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
  else if raw.starts_with?("unsupported_algorithm:")
    UnsupportedAlgorithm(alg: raw.drop_prefix("unsupported_algorithm:") or raw)
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

fn signature_alg_tag(alg: SignatureAlg) : Text
  case alg
    Ed25519: "ed25519"
    Es256Asn1: "es256_asn1"
    Es256Fixed: "es256_fixed"
    Rs256: "rs256"
  end
end

## Verifies a detached digital signature over `message` using `public_key` and `alg`
## (`Ed25519`, P-256 `Es256Asn1`/`Es256Fixed`, or RSA PKCS#1 v1.5 `Rs256`).
fn verify_signature(alg: SignatureAlg, public_key: Bytes, message: Bytes, signature: Bytes) : Result(
  Bool,
  CryptoError
)
  case Crypto.verify_signature_raw(signature_alg_tag(alg), public_key, message, signature)
    Ok(valid?): Ok(valid?)
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

test "RFC 2202 HMAC-SHA1 and verify_signature (Ed25519, ES256, RS256)"
  k1 = must_hex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b")
  assert Crypto.hmac_sha1(k1, "Hi There".bytes).to_hex == "b617318655057264e28bc0b6fb378c8ef146be00"
  assert Crypto.hmac_sha1("Jefe".bytes, "what do ya want for nothing?".bytes).to_hex ==
    "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79"
  k3 = must_hex("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
  d3 = must_hex("dd".repeat(50))
  assert Crypto.hmac_sha1(k3, d3).to_hex == "125d7342b9ac11cd91a39af48aa17b4f63f175d3"
  assert Crypto.hmac_sha1("".bytes, "".bytes).to_hex == "fbdb1d1b18aa6c08324b7d64b71fb76370690e1d"

  ed_pub = must_hex("f347ead52316f7f85f4b0448db694606dfcc1aeb420e13cfdb04933c73be350f")
  ed_sig = must_hex(
    "8a3fc479c36a70d56bb04dc4330886926451a1682f2b236f96c4f725acc98994b437e39ebe9bbf08722d6f0969c7ecc33d8cfe51b1405020052df63374e98a0d"
  )
  assert verify_signature(Ed25519, ed_pub, "arvo-wave0-ed25519".bytes, ed_sig) == Ok(true)
  assert verify_signature(Ed25519, ed_pub, "tampered".bytes, ed_sig) == Ok(false)

  ec_pub = must_hex(
    "04fb4ddba3d0e5b6d1c65eaacd8bf5e13d0e5ab7011cb6591834939df20555dbf8b4a1f9a8726cd2ceff0a1b1cb88b22e637e4c67149b03b0892b7d6c80dfff4ea"
  )
  ec_asn1 = must_hex(
    "304502206a28481e84bc6cbfe1bc232b932f8fc125dca9f1be6e79cdd9b45acbfd08b782022100917dbd45a2dea75974f4aa5147e211dff3ac709da9b5fe9a54cc22ca801e57f1"
  )
  ec_fixed = must_hex(
    "6a28481e84bc6cbfe1bc232b932f8fc125dca9f1be6e79cdd9b45acbfd08b782917dbd45a2dea75974f4aa5147e211dff3ac709da9b5fe9a54cc22ca801e57f1"
  )
  assert verify_signature(Es256Asn1, ec_pub, "arvo-wave0-es256".bytes, ec_asn1) == Ok(true)
  assert verify_signature(Es256Asn1, ec_pub, "tampered".bytes, ec_asn1) == Ok(false)
  assert verify_signature(Es256Fixed, ec_pub, "arvo-wave0-es256".bytes, ec_fixed) == Ok(true)
  assert verify_signature(Es256Fixed, ec_pub, "tampered".bytes, ec_fixed) == Ok(false)

  rsa_pub = must_hex(
    "3082010a0282010100ba7e0e9316a26bf1eba56b750ab96da09a40f275ca1b8c0d971b5bde63c844033fd43bf1f799a57607702c1edc1d9c9c12249f91004be6ee9abd02a8821fd650a2efe27d5f4729b4b341289a6f02075e96c194fbb58bc173cb087fcb5fc4b4aa3e3b0a405843b2acf6ff7797a8f28c9ce64b073c2f3a1216e8b014adbefd98cb1a34d2e02d18efd672c8fc33707abddeb1873ca97d85ceed61bb42acc2447d4e18f3b92d9e9015afc8ce7e2d311d2b3ddaab7597073116e61c176f1fa625a408d44cdcd5d0df7c976ef3f47606997dc4e8dfed3ac675a937dcaee4e717650c6633335a6404d0efeb6c341e7faf09b29c07cb16f5e281b8645f328f7c1f329ba70203010001"
  )
  rsa_sig = must_hex(
    "292dd9caafabdc25759f893c69e15f1f2b5f3b9524d88f4b3e7292d58ba951e778cd5db5b796e3319f43f332697ee3cf4a2a2e41420b4e63c5d22733afe110e27b7a57c93f743825b886007a4f5fa1b8b6487b8e871d0d04d320c3cbec19c38c1196ac16355c8076ba19c874629845b2dd334e22d1ce5d496a7a29f9165a83499cb390073d8eaf3237f3c8be9e657516a6f9749c8f6d5733af37cfb5b69453fc4cf5b09513e3e0676af377cf398b1b30f934d506612af4c27ae23c0a5577466442048751544726d23e9662d5446df495b5868b61b9a1e05f70e2a52b3bdba114ca3bf6a2fc30a82eea53a7c2e16921543aef9c0ab1aff790ba58f124d27c311a"
  )
  assert verify_signature(Rs256, rsa_pub, "arvo-wave0-rsa".bytes, rsa_sig) == Ok(true)
  assert verify_signature(Rs256, rsa_pub, "tampered".bytes, rsa_sig) == Ok(false)
  assert Crypto.verify_signature_raw("bogus", ed_pub, "x".bytes, ed_sig) ==
    Error("unsupported_algorithm:bogus")
end
