module Result
expose map, map_error

## Ok(f(v)) for Ok(v); an Error stays as it is, and f doesn't run.
fn map(result: Result(T, E), f: fn(T) : U) : Result(U, E)
  examples
    Some(2).or_fail("none").map(fn(n) n * 10 end) == Ok(20)
    [1].get(5).or_fail("gone").map(fn(n) n * 10 end) == Error("gone")
  end

  case result
    Ok(v): Ok(f(v))
    Error(e): Error(e)
  end
end

## Error(f(e)) for Error(e); an Ok stays as it is, and f doesn't run.
fn map_error(result: Result(T, E), f: fn(E) : F) : Result(T, F)
  examples
    [1].get(5).or_fail("gone").map_error(fn(t) t.byte_count end) == Error(4)
    Some(2).or_fail("none").map_error(fn(t) t.byte_count end) == Ok(2)
  end

  case result
    Ok(v): Ok(v)
    Error(e): Error(f(e))
  end
end
