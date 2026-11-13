module Main

service
  state Int = 0
  tools read
  token from env "COUNTER_TOKEN"

  promise "the count is never negative 1"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 2"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 3"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 4"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 5"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 6"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 7"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 8"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 9"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 10"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 11"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 12"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 13"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 14"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 15"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 16"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 17"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 18"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 19"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 20"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 21"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 22"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 23"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 24"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 25"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 26"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 27"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 28"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 29"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 30"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 31"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 32"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 33"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 34"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 35"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 36"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 37"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 38"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 39"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 40"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 41"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 42"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 43"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 44"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 45"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 46"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 47"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 48"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 49"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 50"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 51"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 52"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 53"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 54"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 55"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 56"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 57"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 58"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 59"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 60"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 61"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 62"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 63"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 64"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
  promise "the count is never negative 65"
    after >= 0
    allows before: 0, after: 0, now: t0
    forbids before: 0, after: -1, now: t0
  end
end

fn read(state: Int, _now: Time) : Bool
  state >= 0
end

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
