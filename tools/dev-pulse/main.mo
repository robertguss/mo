## Multi-source developer feed aggregator (`dev-pulse`): fetches Hacker News, Lobsters,
## and GitHub trending feeds over HTTPS, canonicalizes and deduplicates cross-posted URLs
## in SQLite, and queries ranked developer trends.
##
##   mo run tools/dev-pulse/main.mo fetch [--db PATH]
##   mo run tools/dev-pulse/main.mo top [--db PATH] [--query WORD] [--limit N]
module Main
use Http
use Json

default_db = "dev-pulse.sqlite"
hn_url = "https://hn.algolia.com/api/v1/search?tags=front_page"
lobsters_url = "https://lobste.rs/hottest.json"
github_url = "https://api.github.com/search/repositories?sort=stars&order=desc&per_page=25&q=stars:%3E100"

struct HnHit
  object_id: Text
  title: Text
  url: Option(Text)
  points: Option(Int)
  num_comments: Option(Int)
end

## Custom Json decoder for HN Algolia hits, mapping camelCase "objectID" to snake_case object_id.
fn from_json(value: Json) : Result(HnHit, Text)
  oid = try value.text_at("objectID").or_fail("expected string field objectID")
  title = try value.text_at("title").or_fail("expected string field title")
  Ok(
    HnHit(
      object_id: oid,
      title: title,
      url: value.text_at("url"),
      points: value.int_at("points"),
      num_comments: value.int_at("num_comments")
    )
  )
end

struct HnFeed
  hits: List(HnHit)
end

struct LobstersStory
  short_id: Text
  title: Text
  url: Text
  score: Int
  comment_count: Int
  tags: List(Text)
end

struct GhRepoItem
  full_name: Text
  html_url: Text
  description: Option(Text)
  stargazers_count: Int
  language: Option(Text)
end

struct GhSearchFeed
  items: List(GhRepoItem)
end

struct NormalizedItem
  canonical: Text
  title: Text
  source: Text
  points: Int
  comments: Int
  tags: Text
end

struct TrendRow
  canonical: Text
  title: Text
  sources: Text
  source_count: Int
  total_score: Int
end

enum PulseCommand
  Fetch(db_path: Text)
  Top(db_path: Text, query: Option(Text), limit: Int)
end

enum PulseError
  Usage(message: Text)
  HttpFailed(source: Text, message: Text)
  DecodeFailed(source: Text, message: Text)
  DbFailed(message: Text)
end

fn error_text(err: PulseError) : Text
  case err
    Usage(message: m): m
    HttpFailed(source: s, message: m): "#{s} fetch failed: #{m}"
    DecodeFailed(source: s, message: m): "#{s} decode failed: #{m}"
    DbFailed(message: m): "sqlite error: #{m}"
  end
end

fn tracking_param?(param: Text) : Bool
  key = (param.split("=").first or "").ascii_lowercase
  key.starts_with?("utm_") or key == "ref" or key == "fbclid" or key == "gclid"
end

## Normalizes a story URL by lowercasing scheme/host, stripping utm_*/ref tracking params,
## and dropping trailing slashes so cross-posted URLs deduplicate reliably.
fn canonical_url(raw: Text) : Text
  promise
    canonical_url(result) == result
  end

  examples
    canonical_url("https://Example.COM/post/?utm_source=hn&id=7") == "https://example.com/post?id=7"
    canonical_url("http://blog.dev/rust-vs-mo/") == "http://blog.dev/rust-vs-mo"
  end

  trimmed = raw.trim
  without_frag = trimmed.split("#").first or trimmed
  qparts = without_frag.split("?")
  base_part = qparts.first or without_frag
  kept_query = if qparts.size > 1
    qraw = qparts.drop(1).join("?")
    kept = qraw.split("&").filter(fn(p) not p.empty? and not tracking_param?(p) end)
    if kept.empty?: "" else: "?#{kept.join("&")}"
  else
    ""
  end
  normalized_base = case base_part.drop_prefix("https://")
    Some(rest): normalize_scheme_rest("https://", rest)
    None:
      case base_part.drop_prefix("http://")
        Some(rest): normalize_scheme_rest("http://", rest)
        None: base_part
      end
  end
  "#{normalized_base}#{kept_query}"
end

fn normalize_scheme_rest(scheme: Text, rest: Text) : Text
  segs = rest.split("/")
  host = (segs.first or "").ascii_lowercase
  tail = segs.drop(1).filter(fn(s) not s.empty? end)
  if tail.empty?
    "#{scheme}#{host}"
  else
    "#{scheme}#{host}/#{tail.join("/")}"
  end
end

## Weighted multi-source trend score with a cross-source corroboration bonus.
fn trend_score(points: Int, comments: Int, source_count: Int) : Int
  promise
    points >= 0 and comments >= 0 and source_count >= 1 implies result >= points
  end

  examples
    trend_score(100, 20, 1) == 140
    trend_score(100, 20, 2) == 215
  end

  p = if points > 0: points else: 0
  c = if comments > 0: comments else: 0
  bonus = if source_count > 1: (source_count - 1).saturating_mul(75) else: 0
  p.saturating_add(c.saturating_mul(2)).saturating_add(bonus)
end

fn parse_top_flags(rest: List(Text), db_path: Text, query: Option(Text), limit: Int) : Result(
  PulseCommand,
  PulseError
)
  case rest
    []: Ok(Top(db_path: db_path, query: query, limit: limit))
    ["--db", p, ..tail] if not p.empty?: parse_top_flags(tail, p, query, limit)
    ["--query", q, ..tail] if not q.empty?: parse_top_flags(tail, db_path, Some(q), limit)
    ["--limit", raw, ..tail]:
      n = try raw.to_int.or_fail(Usage(message: "--limit expects an integer"))
      return Error(Usage(message: "--limit must be between 1 and 100")) if n < 1 or n > 100
      parse_top_flags(tail, db_path, query, n)
    _: Error(Usage(message: "usage: dev-pulse top [--db PATH] [--query WORD] [--limit N]"))
  end
end

fn parse_cli(args: List(Text)) : Result(PulseCommand, PulseError)
  case args
    ["fetch"]: Ok(Fetch(db_path: default_db))
    ["fetch", "--db", p] if not p.empty?: Ok(Fetch(db_path: p))
    ["top", ..rest]: parse_top_flags(rest, default_db, None, 10)
    _: Error(
      Usage(
        message: "usage: dev-pulse fetch [--db PATH] | top [--db PATH] [--query WORD] [--limit N]"
      )
    )
  end
end

fn init_schema(db: Sqlite.Database) : Result(Unit, PulseError)
  s1 = [
    "CREATE TABLE IF NOT EXISTS stories (",
    "  canonical TEXT PRIMARY KEY,",
    "  title TEXT NOT NULL,",
    "  tags TEXT NOT NULL",
    ")"
  ].join("")
  s2 = [
    "CREATE TABLE IF NOT EXISTS mentions (",
    "  canonical TEXT NOT NULL,",
    "  source TEXT NOT NULL,",
    "  points INT NOT NULL,",
    "  comments INT NOT NULL,",
    "  PRIMARY KEY (canonical, source)",
    ")"
  ].join("")
  try db.run(s1, []).map_error(fn(e) DbFailed(message: "#{e}") end)
  db.run(s2, []).map_error(fn(e) DbFailed(message: "#{e}") end)
end

fn upsert_item(db: Sqlite.Database, item: NormalizedItem) : Result(Unit, PulseError)
  s_sql = [
    "INSERT INTO stories (canonical, title, tags) VALUES (?, ?, ?) ",
    "ON CONFLICT(canonical) DO UPDATE SET ",
    "  title = excluded.title,",
    "  tags = CASE WHEN stories.tags = '' THEN excluded.tags ELSE stories.tags END"
  ].join("")
  m_sql = [
    "INSERT INTO mentions (canonical, source, points, comments) VALUES (?, ?, ?, ?) ",
    "ON CONFLICT(canonical, source) DO UPDATE SET ",
    "  points = excluded.points,",
    "  comments = excluded.comments"
  ].join("")
  try db.run(
    s_sql,
    [Sqlite.String(item.canonical), Sqlite.String(item.title), Sqlite.String(item.tags)]
  ).map_error(fn(e) DbFailed(message: "#{e}") end)
  db.run(
    m_sql,
    [
      Sqlite.String(item.canonical),
      Sqlite.String(item.source),
      Sqlite.Integer(item.points),
      Sqlite.Integer(item.comments)
    ]
  ).map_error(fn(e) DbFailed(message: "#{e}") end)
end

fn get_feed_text(net: Network, source: Text, url: Text) : Result(Text, PulseError)
  req = Http.get(url).header("Accept", "application/json").header("User-Agent", "mo-dev-pulse/0.1")
  resp = try req.send(net).map_error(fn(e) HttpFailed(source: source, message: "#{e}") end)
  return Error(HttpFailed(source: source, message: "HTTP #{resp.status}")) if not resp.ok?
  resp.text.or_fail(DecodeFailed(source: source, message: "body is not UTF-8"))
end

fn fetch_all(net: Network, db: Sqlite.Database) : Result(Int, PulseError)
  try init_schema(db)
  scoped = net.allow_hosts(["hn.algolia.com:443", "lobste.rs:443", "api.github.com:443"])

  hn_body = try get_feed_text(scoped, "hn", hn_url)
  hn_feed = try Json.decode[HnFeed](hn_body).map_error(
    fn(e) DecodeFailed(source: "hn", message: "#{e}") end
  )

  lob_body = try get_feed_text(scoped, "lobsters", lobsters_url)
  lob_items = try Json.decode[List(LobstersStory)](lob_body).map_error(
    fn(e) DecodeFailed(source: "lobsters", message: "#{e}") end
  )

  gh_body = try get_feed_text(scoped, "github", github_url)
  gh_feed = try Json.decode[GhSearchFeed](gh_body).map_error(
    fn(e) DecodeFailed(source: "github", message: "#{e}") end
  )

  var count = 0
  for h in hn_feed.hits
    url = h.url or "https://news.ycombinator.com/item?id=#{h.object_id}"
    try upsert_item(
      db,
      NormalizedItem(
        canonical: canonical_url(url),
        title: h.title,
        source: "hn",
        points: h.points or 0,
        comments: h.num_comments or 0,
        tags: ""
      )
    )
    count += 1
  end

  for s in lob_items
    url = if s.url.empty?: "https://lobste.rs/s/#{s.short_id}" else: s.url
    try upsert_item(
      db,
      NormalizedItem(
        canonical: canonical_url(url),
        title: s.title,
        source: "lobsters",
        points: s.score,
        comments: s.comment_count,
        tags: s.tags.join(",")
      )
    )
    count += 1
  end

  for r in gh_feed.items
    lang = (r.language or "").ascii_lowercase
    title = case r.description
      Some(d) if not d.empty?: "#{r.full_name} — #{d}"
      Some(_) | None: r.full_name
    end
    try upsert_item(
      db,
      NormalizedItem(
        canonical: canonical_url(r.html_url),
        title: title,
        source: "github",
        points: r.stargazers_count / 10,
        comments: 0,
        tags: lang
      )
    )
    count += 1
  end
  Ok(count)
end

fn cell_str(row: List(Sqlite.Value), idx: Int) : Text
  if row.get(idx) is Some(Sqlite.String(s)): s else: ""
end

fn cell_int(row: List(Sqlite.Value), idx: Int) : Int
  if row.get(idx) is Some(Sqlite.Integer(n)): n else: 0
end

fn query_top(db: Sqlite.Database, keyword: Option(Text), limit: Int) : Result(
  List(TrendRow),
  PulseError
)
  try init_schema(db)
  sql = [
    "SELECT s.canonical, s.title, s.tags, COUNT(m.source), SUM(m.points), SUM(m.comments), ",
    "GROUP_CONCAT(m.source, '+') ",
    "FROM stories s JOIN mentions m ON s.canonical = m.canonical ",
    "GROUP BY s.canonical, s.title, s.tags"
  ].join("")
  rows = try db.query(sql, []).map_error(fn(e) DbFailed(message: "#{e}") end)
  needle = case keyword
    Some(q): Some(q.ascii_lowercase)
    None: None
  end
  var trends = []
  for row in rows.rows
    canon = cell_str(row, 0)
    title = cell_str(row, 1)
    tags = cell_str(row, 2)
    scount = cell_int(row, 3)
    pts = cell_int(row, 4)
    cmts = cell_int(row, 5)
    srcs = cell_str(row, 6)
    matches = case needle
      Some(k): title.ascii_lowercase.contains?(k) or
        tags.ascii_lowercase.contains?(k) or
        canon.ascii_lowercase.contains?(k)
      None: true
    end
    if matches
      trends = trends.append(
        TrendRow(
          canonical: canon,
          title: title,
          sources: srcs,
          source_count: scount,
          total_score: trend_score(pts, cmts, scount)
        )
      )
    end
  end
  Ok(trends.sort_by_desc(fn(t) t.total_score end).take(limit))
end

fn run_cli(args: Args, net: Network, sqlite: Sqlite, io: Console) : Int
  cmd = case parse_cli(args.list)
    Ok(c): c
    Error(e):
      io.error(error_text(e))
      return 2
  end
  case cmd
    Fetch(db_path: db_path):
      db = case sqlite.open(db_path)
        Ok(d): d
        Error(e):
          io.error("sqlite error: #{e}")
          return 1
      end
      case fetch_all(net, db)
        Ok(count):
          io.print("ingested #{count} feed mentions")
          0
        Error(e):
          io.error(error_text(e))
          1
      end
    Top(db_path: db_path, query: query, limit: limit):
      db = case sqlite.open(db_path)
        Ok(d): d
        Error(e):
          io.error("sqlite error: #{e}")
          return 1
      end
      case query_top(db, query, limit)
        Ok(items):
          for (idx, item) in items.enumerate
            io.print(
              "#{idx + 1}. [#{item.total_score} pts | #{item.sources}] #{item.title} (#{item
                .canonical})"
            )
          end
          0
        Error(e):
          io.error(error_text(e))
          1
      end
  end
end

fn main(args: Args, net: Network, sqlite: Sqlite, io: Console) : Int
  run_cli(args, net, sqlite, io)
end

test "cross-source URL canonicalization, deduplication, and ranked trend search"
  hn_req = Http.get(hn_url).header("Accept", "application/json").header(
    "User-Agent",
    "mo-dev-pulse/0.1"
  )
  lob_req = Http.get(lobsters_url).header("Accept", "application/json").header(
    "User-Agent",
    "mo-dev-pulse/0.1"
  )
  gh_req = Http.get(github_url).header("Accept", "application/json").header(
    "User-Agent",
    "mo-dev-pulse/0.1"
  )

  hn_json = [
    "{\"hits\":[",
    "{\"objectID\":\"1\",\"title\":\"Mo: BEAM Actors + FastAPI\",\"url\":\"https://github.com/robertguss/mo/?utm_source=hn\",\"points\":120,\"num_comments\":30},",
    "{\"objectID\":\"2\",\"title\":\"SQLite WAL Deep Dive\",\"url\":\"https://db.dev/wal/\",\"points\":80,\"num_comments\":10}",
    "]}"
  ].join("")
  lob_json = [
    "[",
    "{\"short_id\":\"abc\",\"title\":\"Mo Language\",\"url\":\"https://GitHub.com/robertguss/mo\",\"score\":45,\"comment_count\":12,\"tags\":[\"compilers\",\"plt\"]}",
    "]"
  ].join("")
  gh_json = [
    "{\"items\":[",
    "{\"full_name\":\"robertguss/mo\",\"html_url\":\"https://github.com/robertguss/mo\",\"description\":\"Living software for agents\",\"stargazers_count\":900,\"language\":\"C\"}",
    "]}"
  ].join("")

  net = Network.fake(
    Http.replies([
      (hn_req, Http.reply(200, hn_json)),
      (lob_req, Http.reply(200, lob_json)),
      (gh_req, Http.reply(200, gh_json))
    ])
  )
  sqlite = Sqlite.in_memory()
  io_fetch = Console.fake()
  assert run_cli(Args.of(["fetch", "--db", "pulse.db"]), net, sqlite, io_fetch) == 0
  assert io_fetch.printed == ["ingested 4 feed mentions"]

  io_top = Console.fake()
  assert run_cli(
    Args.of(["top", "--db", "pulse.db", "--limit", "5"]),
    Network.fake({}),
    sqlite,
    io_top
  ) ==
    0
  assert io_top.printed.size == 2
  assert (io_top.printed.first or "").starts_with?("1. [489 pts | ")
  assert (io_top.printed.first or "").contains?("(https://github.com/robertguss/mo)")

  io_q = Console.fake()
  assert run_cli(
    Args.of(["top", "--db", "pulse.db", "--query", "wal"]),
    Network.fake({}),
    sqlite,
    io_q
  ) ==
    0
  assert io_q.printed == ["1. [100 pts | hn] SQLite WAL Deep Dive (https://db.dev/wal)"]
end

test "empty CLI invocation exits 2"
  io = Console.fake()
  assert run_cli(Args.of([]), Network.fake({}), Sqlite.in_memory(), io) == 2
end
