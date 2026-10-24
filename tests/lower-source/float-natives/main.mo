module Main

absolute = Float.abs
rounded = Float.round

fn probe(n: Float, zero: Float) : Bool
  floor = Float.floor
  ceil = Float.ceil
  sqrt = Float.sqrt
  nan = Float.nan?
  infinite = Float.infinite?
  decimal = Float.to_decimal
  infinity = 1.0 / zero
  not_a_number = zero / zero
  absolute(n) == 2.5 and n.abs == 2.5 and
    n.floor == -3.0 and floor(2.75) == 2.0 and
    n.ceil == -2.0 and ceil(2.25) == 3.0 and
    n.round == -3.0 and rounded(2.5) == 3.0 and
    zero.sqrt == 0.0 and Float.sqrt(9.0) == 3.0 and sqrt(16.0) == 4.0 and
    sqrt(-1.0).nan? and
    nan(not_a_number) and not n.nan? and
    infinity.infinite? and infinite(-infinity) and not infinite(n) and
    not not_a_number.infinite? and not infinity.nan? and
    decimal(0.1) == Some(0.1) and n.to_decimal == Some(-2.5) and
    decimal(not_a_number) is None and infinity.to_decimal is None and
    decimal(-infinity) is None and decimal(1000000000000000000000.0) is None and
    decimal(0.0000000000000000025) == Some(0.000000000000000002) and
    decimal(0.0000000000000000035) == Some(0.000000000000000004)
end

fn main(console: Console) : Int
  console.print("#{probe(-2.5, 0.0)}/#{absolute(-0.0)}/#{rounded(-0.25)}")
  if probe(-2.5, 0.0): 0 else: 1
end
