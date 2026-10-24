## Scratch.
module ScratchCheck
use Calendar

fn slow(from: Int, to: Int) : Int
  var total = 0
  for year in from..to
    total += if Calendar.leap_year?(year): 366 else: 365
  end
  total
end

test "matches the loop"
  for a in -820..820
    for b in [a, a + 1, a + 3, a + 99, a + 100, a + 401, a + 1234]
      assert Calendar.days_between(a, b) == slow(a, b)
    end
  end
  assert Calendar.days_between(-9_223_372_036_854_775_807 - 1, -9_223_372_036_854_775_807) == slow(-9_223_372_036_854_775_807 - 1, -9_223_372_036_854_775_807)
  assert Calendar.days_between(9_223_372_036_854_775_806, 9_223_372_036_854_775_807) == 365
end
