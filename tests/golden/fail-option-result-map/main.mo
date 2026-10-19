module Main

enum ConfigError
  Unreadable(problem: FileError)
  BadPort(text: Text)
end

fn port_of(text: Text) : Result(Int, ConfigError)
  text.trim.to_int.or_fail(BadPort(text: text))
end

fn read_port(files: ReadFiles, path: Text) : Result(Int, ConfigError)
  text = try files.read_text(path).map_error(fn(e) Unreadable(problem: e) end)
  port_of(text)
end

fn explain(problem: ConfigError) : Text
  case problem
    Unreadable(problem: e): "can't read the config: #{e}"
    BadPort(text: t): "not a port: #{t.trim}"
  end
end

fn main(console: Console, files: ReadFiles)
  dir = "tests/golden/fail-option-result-map/data"
  for name in ["good.txt", "bad.txt", "gone.txt"]
    read = read_port(files, "#{dir}/#{name}")
    shown = read.map(fn(port) "port #{port + 1}" end).map_error(explain)
    console.print("#{name}: #{shown}")
  end
  console.print("#{Some(3).map(fn(n) n * n end)} #{"x".to_int.map(fn(n) n * n end)}")
  console.print("#{NotFound(path: "a.md")}, #{Failed(path: "b", reason: "is a folder").to_text}")
end
