service
  state List(Int) = # initializer
    [1, 2]
  tools state, # comma
    tools, every, token, from, env
  every 1. # duration
    minute env # handler
  token from env "TOKEN" # source
end # close
