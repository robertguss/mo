## A stand-in for the proposed HTTP client library, with its signatures and no bodies.
module Http
expose Request(..),
Response(..),
HttpError(..),
get,
post,
request,
header,
query,
json,
text,
within,
send,
ok?,
replies,
reply
use Json
use Network{Network, NetworkError}

struct Request
  method: Text
  url: Text
  headers: List((Text, Text))
  body: Bytes
  timeout: Duration
end

struct Response
  status: Int
  headers: List((Text, Text))
  body: Bytes
end

enum HttpError
  BadUrl(url: Text)
  Unreachable(NetworkError)
  BadResponse(reason: Text)
  NotJson(Json.JsonError)
  TooLarge(limit: Int)
end

fn get(url: Text) : Request
  request("GET", url)
end

fn post(url: Text) : Request
  request("POST", url)
end

fn request(method: Text, url: Text) : Request
  Request(method: method, url: url, headers: [], body: "".bytes, timeout: 30.seconds)
end

fn header(_request: Request, _name: Text, _value: Text) : Request
  stop "a stand-in"
end

fn header(_response: Response, _name: Text) : Option(Text)
  stop "a stand-in"
end

fn query(_request: Request, _name: Text, _value: Text) : Request
  stop "a stand-in"
end

fn json(_request: Request, _value: Json) : Request
  stop "a stand-in"
end

fn json(_response: Response) : Result(Json, HttpError)
  stop "a stand-in"
end

fn text(_request: Request, _text: Text) : Request
  stop "a stand-in"
end

fn text(_response: Response) : Option(Text)
  stop "a stand-in"
end

fn within(_request: Request, _timeout: Duration) : Request
  stop "a stand-in"
end

fn send(_request: Request, _network: Network) : Result(Response, HttpError)
  stop "a stand-in"
end

fn ok?(response: Response) : Bool
  response.status >= 200 and response.status < 300
end

fn replies(_exchanges: List((Request, Response))) : Map(Text, Map(Bytes, Bytes))
  stop "a stand-in"
end

fn reply(status: Int, body: Text) : Response
  Response(status: status, headers: [], body: body.bytes)
end
