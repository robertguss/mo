module Main

service
  state Int = 0
  tools echo
  token from env "TOKEN"

  promise "counting empty consoles is allowed"
    after >= before and count(empty_consoles()) == 1
    allows before: 0, after: 0, now: t0
    forbids before: 1, after: 0, now: t0
  end
end

fn echo(state: Int, _now: Time) : (Int, Unit)
  (state, Unit)
end

fn empty_consoles() : List(Console)
  []
end

fn count(items: List(T)) : Int
  f = fn(extra) items.size + extra end
  f(1)
end

t0 = Time.parse("2026-01-01T00:00:00Z") or stop "time"
