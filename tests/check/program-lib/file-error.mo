module FileError

fn to_text(error: FileError) : Text
  case error
    NotFound(path: path): "not found: #{path}"
    NotAllowed(path: path): "not allowed: #{path}"
    NotText(path: path): "not text: #{path}"
    Failed(reason: reason): reason
  end
end
