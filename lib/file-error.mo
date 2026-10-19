module FileError
expose to_text

## The error as a person reads it: the path, then what went wrong.
fn to_text(error: FileError) : Text
  examples
    NotFound(path: "notes/a.md").to_text == "notes/a.md: no such file"
    NotAllowed(path: "../a.txt").to_text == "../a.txt: not allowed"
    NotText(path: "bin/app").to_text == "bin/app: not text"
    Failed(path: "out.txt", reason: "is a folder").to_text == "out.txt: is a folder"
  end

  case error
    NotFound(path: path): "#{path}: no such file"
    NotAllowed(path: path): "#{path}: not allowed"
    NotText(path: path): "#{path}: not text"
    Failed(path: path, reason: reason): "#{path}: #{reason}"
  end
end
