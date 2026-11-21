## GitHub repository issue and pull-request sync into SQLite, with cycle-time analytics.
##
##   mo run tools/gh-sync/main.mo sync OWNER/REPO [--db PATH] [--pages N]
##   mo run tools/gh-sync/main.mo stats OWNER/REPO [--db PATH]
module Main
use Http
use Json

default_db = "gh-sync.sqlite"

struct GhUser
  login: Text
end

struct GhPullRef
  url: Text
end

struct GhItem
  id: Int
  number: Int
  title: Text
  state: Text
  user: GhUser
  created_at: Time
  closed_at: Option(Time)
  pull_request: Option(GhPullRef)
end

struct RepoStats
  repo: Text
  open_issues: Int
  closed_issues: Int
  open_prs: Int
  closed_prs: Int
  mean_cycle_hours: Option(Decimal)
  median_cycle_hours: Option(Decimal)
end

enum SyncCommand
  Sync(repo: Text, db_path: Text, max_pages: Int)
  Stats(repo: Text, db_path: Text)
end

enum SyncError
  Usage(message: Text)
  BadRepo(value: Text)
  HttpFailed(message: Text)
  DecodeFailed(message: Text)
  DbFailed(message: Text)
end

fn error_text(err: SyncError) : Text
  case err
    Usage(message: m): m
    BadRepo(value: v): "invalid repository '#{v}'; expected OWNER/REPO"
    HttpFailed(message: m): "github request failed: #{m}"
    DecodeFailed(message: m): "github JSON decode failed: #{m}"
    DbFailed(message: m): "sqlite error: #{m}"
  end
end

fn valid_segment?(seg: Text) : Bool
  return false if seg.empty?
  seg.chars.all?(fn(c) c.ascii_letter? or c.ascii_digit? or c == '-' or c == '_' or c == '.' end) and
    seg != "." and
    seg != ".."
end

## Validates an OWNER/REPO identifier.
fn parse_repo(raw: Text) : Result((Text, Text), SyncError)
  promise
    result is Ok((owner, name)) implies not owner.empty? and not name.empty?
  end

  examples
    parse_repo("robertguss/mo") == Ok(("robertguss", "mo"))
    parse_repo("invalid") == Error(BadRepo(value: "invalid"))
    parse_repo("a/b/c") == Error(BadRepo(value: "a/b/c"))
  end

  parts = raw.trim.split("/")
  case parts
    [owner, name] if valid_segment?(owner) and valid_segment?(name): Ok((owner, name))
    _: Error(BadRepo(value: raw))
  end
end

fn parse_flags(rest: List(Text), db_path: Text, max_pages: Int) : Result((Text, Int), SyncError)
  case rest
    []: Ok((db_path, max_pages))
    ["--db", path, ..tail] if not path.empty?: parse_flags(tail, path, max_pages)
    ["--pages", raw, ..tail]:
      pages = try raw.to_int.or_fail(Usage(message: "--pages expects a positive integer"))
      return Error(Usage(message: "--pages must be between 1 and 100")) if pages < 1 or pages > 100
      parse_flags(tail, db_path, pages)
    _: Error(Usage(message: "usage: gh-sync sync|stats OWNER/REPO [--db PATH] [--pages N]"))
  end
end

fn parse_cli(args: List(Text)) : Result(SyncCommand, SyncError)
  examples
    parse_cli(["sync", "robertguss/mo"]) ==
      Ok(Sync(repo: "robertguss/mo", db_path: "gh-sync.sqlite", max_pages: 10))
    parse_cli(["stats", "robertguss/mo", "--db", "local.db"]) ==
      Ok(Stats(repo: "robertguss/mo", db_path: "local.db"))
  end

  case args
    ["sync", repo_arg, ..rest]:
      (owner, name) = try parse_repo(repo_arg)
      (db_path, max_pages) = try parse_flags(rest, default_db, 10)
      Ok(Sync(repo: "#{owner}/#{name}", db_path: db_path, max_pages: max_pages))
    ["stats", repo_arg, ..rest]:
      (owner, name) = try parse_repo(repo_arg)
      (db_path, _) = try parse_flags(rest, default_db, 10)
      Ok(Stats(repo: "#{owner}/#{name}", db_path: db_path))
    _: Error(Usage(message: "usage: gh-sync sync|stats OWNER/REPO [--db PATH] [--pages N]"))
  end
end

## Nonnegative cycle time in hours between created_at and closed_at, rounded to 2 places.
fn cycle_hours(created: Time, closed: Time) : Option(Decimal)
  promise
    result is Some(hours) implies hours >= 0.0
    closed < created implies result == None
  end

  return None if closed < created
  secs = (closed - created).in_seconds
  Some((secs / 3600.0).round(2))
end

fn median_decimal(values: List(Decimal)) : Option(Decimal)
  examples
    median_decimal([]) == None
    median_decimal([10.0, 2.0, 6.0]) == Some(6.0)
    median_decimal([1.0, 3.0, 5.0, 7.0]) == Some(4.0)
  end

  return None if values.empty?
  sorted = values.sort
  mid = sorted.size / 2
  if sorted.size % 2 == 1
    sorted.get(mid)
  else
    left = sorted.get(mid - 1) or 0.0
    right = sorted.get(mid) or 0.0
    Some(((left + right) / 2.0).round(2))
  end
end

fn init_schema(db: Sqlite.Database) : Result(Unit, SyncError)
  stmt = [
    "CREATE TABLE IF NOT EXISTS gh_items (",
    "  repo TEXT NOT NULL,",
    "  number INT NOT NULL,",
    "  gh_id INT NOT NULL,",
    "  kind TEXT NOT NULL,",
    "  state TEXT NOT NULL,",
    "  author TEXT NOT NULL,",
    "  title TEXT NOT NULL,",
    "  created_at TEXT NOT NULL,",
    "  closed_at TEXT,",
    "  PRIMARY KEY (repo, number)",
    ")"
  ].join("")
  db.run(stmt, []).map_error(fn(e) DbFailed(message: "#{e}") end)
end

fn build_page_request(repo: Text, page: Int, token: Option(Text)) : Http.Request
  base = Http.get("https://api.github.com/repos/#{repo}/issues").query("state", "all").query(
    "per_page",
    "100"
  ).query("page", "#{page}").header("Accept", "application/vnd.github+json").header(
    "User-Agent",
    "mo-gh-sync/0.1"
  )
  case token
    Some(tok) if not tok.trim.empty?: base.header("Authorization", "Bearer #{tok.trim}")
    Some(_) | None: base
  end
end

fn upsert_item(db: Sqlite.Database, repo: Text, item: GhItem) : Result(Unit, SyncError)
  kind = if item.pull_request is Some(_): "pr" else: "issue"
  closed_val = case item.closed_at
    Some(t): Sqlite.String(t.to_text)
    None: Sqlite.Null
  end
  sql = [
    "INSERT INTO gh_items (repo, number, gh_id, kind, state, author, title, created_at, closed_at) ",
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) ",
    "ON CONFLICT(repo, number) DO UPDATE SET ",
    "  gh_id = excluded.gh_id,",
    "  kind = excluded.kind,",
    "  state = excluded.state,",
    "  author = excluded.author,",
    "  title = excluded.title,",
    "  created_at = excluded.created_at,",
    "  closed_at = excluded.closed_at"
  ].join("")
  params = [
    Sqlite.String(repo),
    Sqlite.Integer(item.number),
    Sqlite.Integer(item.id),
    Sqlite.String(kind),
    Sqlite.String(item.state),
    Sqlite.String(item.user.login),
    Sqlite.String(item.title),
    Sqlite.String(item.created_at.to_text),
    closed_val
  ]
  db.run(sql, params).map_error(fn(e) DbFailed(message: "#{e}") end)
end

fn sync_repo(net: Network, db: Sqlite.Database, repo: Text, token: Option(Text), max_pages: Int) : Result(
  Int,
  SyncError
)
  try init_schema(db)
  scoped = net.allow_hosts(["api.github.com:443"])
  var page = 1
  var synced = 0
  while page <= max_pages
    req = build_page_request(repo, page, token)
    resp = try req.send(scoped).map_error(fn(e) HttpFailed(message: "#{e}") end)
    return Error(HttpFailed(message: "HTTP #{resp.status}")) if not resp.ok?
    body_text = try resp.text.or_fail(DecodeFailed(message: "response body is not UTF-8"))
    items = try Json.decode[List(GhItem)](body_text).map_error(
      fn(e) DecodeFailed(message: "#{e}") end
    )
    break if items.empty?
    for item in items
      try upsert_item(db, repo, item)
      synced += 1
    end
    break if items.size < 100
    page += 1
  end
  Ok(synced)
end

fn cell_text(row: List(Sqlite.Value), idx: Int) : Option(Text)
  if row.get(idx) is Some(Sqlite.String(s)): Some(s) else: None
end

fn compute_stats(db: Sqlite.Database, repo: Text) : Result(RepoStats, SyncError)
  try init_schema(db)
  sql = "SELECT kind, state, created_at, closed_at FROM gh_items WHERE repo = ? ORDER BY number ASC"
  rows = try db.query(sql, [Sqlite.String(repo)]).map_error(fn(e) DbFailed(message: "#{e}") end)
  var open_issues = 0
  var closed_issues = 0
  var open_prs = 0
  var closed_prs = 0
  var cycles = []
  for row in rows.rows
    kind = cell_text(row, 0) or "issue"
    state = cell_text(row, 1) or "open"
    if kind == "pr"
      if state == "closed"
        closed_prs += 1
      else
        open_prs += 1
      end
    else
      if state == "closed"
        closed_issues += 1
      else
        open_issues += 1
      end
    end
    case (cell_text(row, 2), cell_text(row, 3))
      (Some(c_raw), Some(cl_raw)):
        case (Time.parse(c_raw), Time.parse(cl_raw))
          (Some(created), Some(closed)):
            case cycle_hours(created, closed)
              Some(h): cycles = cycles.append(h)
              None: Unit
            end
          _: Unit
        end
      _: Unit
    end
  end
  mean = if cycles.empty?
    None
  else
    Some((cycles.sum / cycles.size.to_decimal).round(2))
  end
  Ok(
    RepoStats(
      repo: repo,
      open_issues: open_issues,
      closed_issues: closed_issues,
      open_prs: open_prs,
      closed_prs: closed_prs,
      mean_cycle_hours: mean,
      median_cycle_hours: median_decimal(cycles)
    )
  )
end

fn format_stats(s: RepoStats) : Text
  mean_txt = case s.mean_cycle_hours
    Some(d): "#{d.to_fixed(2)}h"
    None: "n/a"
  end
  med_txt = case s.median_cycle_hours
    Some(d): "#{d.to_fixed(2)}h"
    None: "n/a"
  end
  [
    "repo: #{s.repo}",
    "issues: #{s.open_issues} open, #{s.closed_issues} closed",
    "pulls: #{s.open_prs} open, #{s.closed_prs} closed",
    "cycle_time: mean #{mean_txt}, median #{med_txt}"
  ].join("\n")
end

fn run_cli(args: Args, env: Env, net: Network, sqlite: Sqlite, io: Console) : Int
  cmd = case parse_cli(args.list)
    Ok(c): c
    Error(e):
      io.error(error_text(e))
      return 2
  end
  case cmd
    Sync(repo: repo, db_path: db_path, max_pages: max_pages):
      db = case sqlite.open(db_path)
        Ok(d): d
        Error(e):
          io.error("sqlite error: #{e}")
          return 1
      end
      case sync_repo(net, db, repo, env.get("GITHUB_TOKEN"), max_pages)
        Ok(count):
          io.print("synced #{count} items for #{repo}")
          0
        Error(e):
          io.error(error_text(e))
          1
      end
    Stats(repo: repo, db_path: db_path):
      db = case sqlite.open(db_path)
        Ok(d): d
        Error(e):
          io.error("sqlite error: #{e}")
          return 1
      end
      case compute_stats(db, repo)
        Ok(stats):
          io.print(format_stats(stats))
          0
        Error(e):
          io.error(error_text(e))
          1
      end
  end
end

fn main(args: Args, env: Env, net: Network, sqlite: Sqlite, io: Console) : Int
  run_cli(args, env, net, sqlite, io)
end

test "incremental github sync, wide JSON tolerance, and cycle-time analytics"
  page1_req = build_page_request("robertguss/mo", 1, Some("ghp_test"))
  page1_json = [
    "[",
    "{\"id\":101,\"number\":1,\"title\":\"Initial parser\",\"state\":\"closed\",",
    "\"node_id\":\"MDU6SXNzdWUx\",\"html_url\":\"https://github.com/robertguss/mo/issues/1\",",
    "\"user\":{\"login\":\"ada\",\"site_admin\":false},",
    "\"created_at\":\"2026-10-01T00:00:00Z\",\"closed_at\":\"2026-10-01T06:00:00Z\",\"pull_request\":null},",
    "{\"id\":102,\"number\":2,\"title\":\"Add actors\",\"state\":\"closed\",",
    "\"user\":{\"login\":\"grace\"},",
    "\"created_at\":\"2026-10-02T00:00:00Z\",\"closed_at\":\"2026-10-02T18:00:00Z\",",
    "\"pull_request\":{\"url\":\"https://api.github.com/repos/robertguss/mo/pulls/2\"}},",
    "{\"id\":103,\"number\":3,\"title\":\"Crucible suite\",\"state\":\"open\",",
    "\"user\":{\"login\":\"linus\"},",
    "\"created_at\":\"2026-10-03T00:00:00Z\",\"closed_at\":null}",
    "]"
  ].join("")
  net1 = Network.fake(Http.replies([(page1_req, Http.reply(200, page1_json))]))
  sqlite = Sqlite.in_memory()
  env = Env.of({"GITHUB_TOKEN": "ghp_test"})
  io1 = Console.fake()
  assert run_cli(Args.of(["sync", "robertguss/mo", "--db", "gh.db"]), env, net1, sqlite, io1) == 0
  assert io1.printed == ["synced 3 items for robertguss/mo"]

  update_json = [
    "[",
    "{\"id\":103,\"number\":3,\"title\":\"Crucible suite\",\"state\":\"closed\",",
    "\"user\":{\"login\":\"linus\"},",
    "\"created_at\":\"2026-10-03T00:00:00Z\",\"closed_at\":\"2026-10-03T12:00:00Z\"}",
    "]"
  ].join("")
  net2 = Network.fake(Http.replies([(page1_req, Http.reply(200, update_json))]))
  io2 = Console.fake()
  assert run_cli(Args.of(["sync", "robertguss/mo", "--db", "gh.db"]), env, net2, sqlite, io2) == 0
  assert io2.printed == ["synced 1 items for robertguss/mo"]

  io3 = Console.fake()
  assert run_cli(
    Args.of(["stats", "robertguss/mo", "--db", "gh.db"]),
    env,
    Network.fake({}),
    sqlite,
    io3
  ) ==
    0
  assert io3.printed == [
    [
      "repo: robertguss/mo",
      "issues: 0 open, 2 closed",
      "pulls: 0 open, 1 closed",
      "cycle_time: mean 12.00h, median 12.00h"
    ].join("\n")
  ]
end

test "empty CLI invocation exits 2 deterministically"
  io = Console.fake()
  status = run_cli(Args.of([]), Env.of({}), Network.fake({}), Sqlite.in_memory(), io)
  assert status == 2
  assert io.printed_errors.size == 1
end
