## Syntax keeps clause order and repetition for the checker.
service                   # opener
  token from env "TOKEN"  # source

  # initial state
  state List(Int) = [
    1,  # value
    2
  ]
  # tools
  tools state, tools, every, token, from, env  # handler
  # timer
  every interval(from) + 1.minute token  # handler
  # repeated clauses
  state (Int, Text) = (0, "zero")
  state fn(Int) : Int = fn(state) state end
  state Other.State = Other.empty

  every if ready: first else: second env
  tools service?
  token from env """
    TOKEN
    """
  # end of service
end  # close

service
end
