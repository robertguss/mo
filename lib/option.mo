module Option
expose or_fail, map

## Some(v) becomes Ok(v), and None becomes Error(e). The error is computed either way.
fn or_fail(value: Option(T), e: E) : Result(T, E)
  examples
    Some(1).or_fail("none") == Ok(1)
    [1].get(5).or_fail("gone") == Error("gone")
  end

  case value
    Some(v): Ok(v)
    None: Error(e)
  end
end

## Some(f(v)) for Some(v), and None for None; f runs only on a value.
fn map(value: Option(T), f: fn(T) : U) : Option(U)
  examples
    Some(2).map(fn(n) n * 10 end) == Some(20)
    [1].get(5).map(fn(n) n * 10 end) == None
    Some("ab").map(fn(t) t.byte_count end) == Some(2)
  end

  case value
    Some(v): Some(f(v))
    None: None
  end
end
