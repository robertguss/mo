module ScratchCheck
use Calendar

fn slow(from: Int, to: Int) : Int
  var total = 0
  for y in from..to
    total += if Calendar.leap_year?(y): 366 else: 365
  end
  total
end

test "formula matches the loop"
  var bad = 0
  for a in -850..850
    for d in [0, 1, 3, 4, 99, 100, 101, 399, 400, 401, 1000]
      bad += if Calendar.days_between(a, a + d) == slow(a, a + d): 0 else: 1
    end
  end
  assert bad == 0
  assert not Calendar.leap_year?(-100)
  assert Calendar.leap_year?(-400)
  assert Calendar.leap_year?(0)
end
