module Option
expose or_fail

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
