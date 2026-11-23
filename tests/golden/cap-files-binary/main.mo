module Main

test "bytes binary packing and unpacking"
  assert Bytes.empty.size == 0
  assert Bytes.zeroed(0) == Bytes.empty
  assert Bytes.zeroed(4) == Bytes.from_list([0, 0, 0, 0])
  raw = Bytes.from_list([0, 1, 127, 128, 255])
  assert raw.to_list == [0, 1, 127, 128, 255]
  assert Bytes.from_u16_be(0) == Bytes.from_list([0, 0])
  assert Bytes.from_u16_be(65535) == Bytes.from_list([255, 255])
  assert Bytes.from_u32_be(2147483649) == Bytes.from_list([128, 0, 0, 1])
  assert Bytes.from_u32_be(4294967295) == Bytes.from_list([255, 255, 255, 255])
  assert Bytes.from_i64_be(-1) == Bytes.from_list([255, 255, 255, 255, 255, 255, 255, 255])
  assert Bytes.from_i64_be(-9223372036854775808) == Bytes.from_list([128, 0, 0, 0, 0, 0, 0, 0])
  frame = Bytes.join([
    Bytes.from_u16_be(513),
    Bytes.from_u32_be(305419896),
    Bytes.from_i64_be(-9223372036854775808)
  ])
  assert frame.u16_be_at(0) == Some(513)
  assert frame.u32_be_at(2) == Some(305419896)
  assert frame.i64_be_at(6) == Some(-9223372036854775808)
  assert frame.u16_be_at(-1) == None
  assert frame.u16_be_at(13) == None
  assert frame.u32_be_at(11) == None
  assert frame.i64_be_at(7) == None
  assert frame.i64_be_at(9223372036854775807) == None
end

test "binary files write_bytes rename and random access handle in memory"
  files = Files.in_memory({})
  raw = Bytes.from_list([0, 255, 192, 128])
  assert files.write_bytes("raw.bin", raw) == Ok(Unit)
  assert files.read_bytes("raw.bin") == Ok(raw)
  assert files.read_text("raw.bin") == Error(NotText(path: "raw.bin"))
  assert files.rename("raw.bin", "db.bin") == Ok(Unit)
  assert files.read_bytes("raw.bin") == Error(NotFound(path: "raw.bin"))
  assert files.open("db.bin") is Ok(fh)
  assert fh.size == Ok(4)
  assert fh.write_at(6, Bytes.from_u16_be(4660)) == Ok(Unit)
  assert fh.size == Ok(8)
  assert fh.read_at(0, 8) == Ok(Bytes.from_list([0, 255, 192, 128, 0, 0, 18, 52]))
  assert fh.read_at(6, 100) == Ok(Bytes.from_list([18, 52]))
  assert fh.read_at(8, 10) == Ok(Bytes.empty)
  assert fh.truncate(3) == Ok(Unit)
  assert fh.truncate(5) == Ok(Unit)
  assert fh.sync == Ok(Unit)
  assert files.read_bytes("db.bin") == Ok(Bytes.from_list([0, 255, 192, 0, 0]))
end
