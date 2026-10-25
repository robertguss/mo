module Main
use Helpers { snapshot }
service
  state Int = 0
  tools snapshot
  token from env "TOKEN"
end
