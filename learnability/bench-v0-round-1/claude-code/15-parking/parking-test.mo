## Tests of the Parking module.
module ParkingTest
use Parking

test "a short stay is free"
  assert Parking.fee(0) == Some(0)
  assert Parking.fee(10) == Some(0)
  assert Parking.fee(30) == Some(0)
end

test "every started hour is paid"
  assert Parking.fee(31) == Some(2.5)
  assert Parking.fee(59) == Some(2.5)
  assert Parking.fee(60) == Some(2.5)
  assert Parking.fee(61) == Some(5)
  assert Parking.fee(179) == Some(7.5)
end

test "a day costs at most the daily cap"
  assert Parking.fee(480) == Some(20)
  assert Parking.fee(600) == Some(20)
  assert Parking.fee(1440) == Some(20)
end

test "each day is capped on its own"
  assert Parking.fee(2880) == Some(40)
  assert Parking.fee(2880 + 600) == Some(60)
end

test "a stay can't be negative"
  assert Parking.fee(-1) == None
  assert Parking.receipt(-1) == None
end

test "a receipt gives the stay and the fee"
  assert Parking.receipt(15) == Some("0h 15m: 0.00")
  assert Parking.receipt(120) == Some("2h 00m: 5.00")
  assert Parking.receipt(1500) == Some("25h 00m: 22.50")
end
