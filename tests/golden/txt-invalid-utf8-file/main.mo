module Main

fn main(console: Console, files: ReadFiles)
  path = "tests/golden/txt-invalid-utf8-file/data/latin1.txt"
  case files.read_bytes(path)
    Ok(bytes): console.print("#{bytes.size} #{bytes.get(2)} #{bytes.decode_utf8}")
    Error(problem): console.print("unexpected #{problem}")
  end
  case files.read_text(path)
    Ok(text): console.print("unexpected #{text}")
    Error(problem): console.print("#{problem} | #{problem == NotText(path: path)}")
  end
  console.print("#{"caf\u{E9}".bytes.decode_utf8}")
end
