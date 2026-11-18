## Saves a repository's open issues in a SQLite database and prints their titles.
##
##   issues OWNER/REPO
##
## The token is read from the environment variable API_TOKEN.
module Issues
use Http{HttpError}
use JsonPath{int_at, list_at, text_at}
use Network{Network}
use Sqlite{Sqlite, SqliteError}

enum Halt
  Usage
  Call(HttpError)
  Database(SqliteError)
  Unexpected(Text)
end

fn tool(args: Args, env: Env, console: Console, network: Network, sqlite: Sqlite) : Result(
  Unit,
  Halt
)
  repo = try args.list.first.or_fail(Usage)
  database = try sqlite.open("issues.db")
  saved = try sync(network, database, repo, env.get("API_TOKEN") or "")
  console.print("saved #{saved} open issues")
  found = try database.query("select number, title from issues order by number", [])
  for row in found.rows
    console.print("#{row}")
  end
  Ok(Unit)
end

## Fetches the open issues of `repo` and saves each one. Gives the number saved.
fn sync(network: Network, database: Sqlite.Database, repo: Text, token: Text) : Result(Int, Halt)
  url = "https://api.example.com/repos/#{repo}/issues"
  call = Http.get(url).query("state", "open").header("Authorization", "Bearer #{token}")
  response = try call.send(network)
  return Error(Unexpected("the API answered #{response.status}")) if not response.ok?
  issues = try response.json
  try database.run(
    "create table if not exists issues (number integer primary key, title text not null)",
    []
  )
  var saved = 0
  for issue in issues.list_at("") or []
    number = try issue.int_at("number").or_fail(Unexpected("an issue has no number"))
    title = try issue.text_at("title").or_fail(Unexpected("issue #{number} has no title"))
    try database.run(
      "insert or replace into issues (number, title) values (?, ?)",
      [Sqlite.Integer(number), Sqlite.String(title)]
    )
    saved += 1
  end
  Ok(saved)
end

test "sync saves each open issue, with no network"
  page = "[{\"number\": 7, \"title\": \"Crash on empty input\"}, {\"number\": 9, \"title\": \"Slow start\"}]"
  url = "https://api.example.com/repos/mo/mo/issues"
  asked = Http.get(url).query("state", "open").header("Authorization", "Bearer t0ken")
  network = Network.fake(Http.replies([(asked, Http.reply(200, page))]))
  assert Sqlite.fake().open("issues.db") is Ok(database)
  assert sync(network, database, "mo/mo", "t0ken") == Ok(2)
  assert database.query("select title from issues order by number", []) is Ok(found)
  assert found.rows == [[Sqlite.String("Crash on empty input")], [Sqlite.String("Slow start")]]
end

test "a request the fake holds no reply for is an error"
  network = Network.fake(Http.replies([]))
  assert Sqlite.fake().open("issues.db") is Ok(database)
  assert sync(network, database, "mo/mo", "t0ken") is Error(Call(_))
end
