module Control

fn f(c: Bool): Int
  return if c: 1 else: 2
  return if c: 3 else if c: 4 else: 5 if c
  return 6 if c
  return if c
end

fn arms(value: Option(Int))
  case value
    Some(x) if x > 0: return x
    None | Some(-1):
      for i in 0..3
        i
      end
    Some(_): 0
  end
end
