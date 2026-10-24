## Scratch.
module ScratchCheck
use Calendar

test "closed form matches a loop"
  var total = 0
  for y in -2000..2500
    assert Calendar.days_between(-2000, y) == total
    total += if Calendar.leap_year?(y): 366 else: 365
  end
end
