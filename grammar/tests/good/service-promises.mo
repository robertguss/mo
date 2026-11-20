## Service promises follow token; on names a tool and stays an ordinary name elsewhere.
service
  state Int = 0
  tools add
  token from env "TOKEN"

  promise
    after >= before
  end

  promise on add  # tool promise
    reply implies after == before + on
  end
end

on = 1

fn on(on: Int) : Int
  promise "on is a name" on on
    result == on
  end

  on
end
