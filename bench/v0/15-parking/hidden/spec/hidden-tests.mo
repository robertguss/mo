## The hidden tests of task 15-parking.
module HiddenTests
use Parking

test "each started hour after the free half hour costs 2.50"
  for hours in 1..8
    assert Parking.fee(hours * 60) == Some(2.5 * hours.to_decimal)
    assert Parking.fee(hours * 60 + 1) == Some(2.5 * (hours + 1).to_decimal)
  end
end

test "a started hour on a later day is paid"
  assert Parking.fee(1441) == Some(22.5)
  assert Parking.fee(1440 + 61) == Some(25)
  assert Parking.fee(2 * 1440 + 1) == Some(42.5)
end

test "the cap starts at the eighth hour"
  assert Parking.fee(7 * 60) == Some(17.5)
  assert Parking.fee(7 * 60 + 1) == Some(20)
end

test "receipts round up their minutes into hours"
  assert Parking.receipt(125) == Some("2h 05m: 7.50")
  assert Parking.receipt(31) == Some("0h 31m: 2.50")
end
