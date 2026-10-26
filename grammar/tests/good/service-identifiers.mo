module Names
expose state, tools, every, token, from, env
use Other{state, tools, every, token, from, env}

state = tools.every(token, from.env)
tools = state
every = tools
token = every
from = token
env = from

struct Names
  state: Int
  tools: Int
  every: Int
  token: Int
  from: Int
  env: Int
end

enum Choice
  Named(state: Int, tools: Int, every: Int, token: Int, from: Int, env: Int)
end

fn state(state: Int, tools: Int, every: Int, token: Int, from: Int, env: Int) : Int
  promise "names"
    state == tools
    allows state: 0, tools: 0, every: 0, token: 0, from: 0, env: 0
    forbids state: 0, tools: 1, every: 0, token: 0, from: 0, env: 0
  end

  var state = tools
  var tools = every
  var every = token
  var token = from
  var from = env
  var env = state
  for (state, tools, every, token, from, env) in rows
    env
  end
  Names(state: state, tools: tools, every: every, token: token, from: from, env: env)
  (fn(state, tools, every, token, from, env) [state, tools, every, token, from, env] end)
  fn(state) [state] end
  fn(tools) [tools] end
  fn(every) [every] end
  fn(token) [token] end
  fn(from) [from] end
  fn(env) [env] end
  state.tools.every.token.from.env
  state
  tools
  every
  token
  from
  env
end

fn tools()
end
fn every()
end
fn token()
end
fn from()
end
fn env()
end
fn service?(state?: Int) : Int
  state?
end
