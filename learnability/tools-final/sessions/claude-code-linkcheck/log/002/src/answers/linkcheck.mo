## Checks that every relative link in the Markdown files below a folder leads somewhere.
## Usage: linkcheck [FOLDER]
module Linkcheck

use Unicode16

struct Ref
  line: Int
  target: Text
end

struct Broken
  path: Text
  line: Int
  target: Text
  reason: Text
end

fn run_length(cs: List(Char), start: Int, c: Char) : Int
  var n = 0
  while cs.get(start + n) == Some(c)
    n += 1
  end
  n
end

## A fence line: its character, its length and the text after it.
fn fence(line: Text) : Option((Char, Int, Text))
  examples
    fence("```mo") == Some(('`', 3, "mo"))
    fence("  ~~~~") == Some(('~', 4, ""))
    fence("    ```") == None
    fence("``") == None
    fence("``` a`b") == None
  end

  cs = line.chars
  indent = run_length(cs, 0, ' ')
  return None if indent > 3
  c = cs.get(indent) or ' '
  return None if c != '`' and c != '~'
  n = run_length(cs, indent, c)
  return None if n < 3
  rest = Text.from_chars(cs.drop(indent + n)).trim
  return None if c == '`' and rest.contains?("`")
  Some((c, n, rest))
end

## Marks the lines inside fenced code blocks, fences included.
fn code_lines(lines: List(Text)) : List(Bool)
  examples
    code_lines(["a", "```", "b", "~~~", "```", "c"]) == [false, true, true, true, true, false]
    code_lines(["~~~~", "~~~", "~~~~~", "x"]) == [true, true, true, false]
  end

  var marks = []
  var open = None
  for line in lines
    f = fence(line)
    case open
      None:
        if f is Some((c, n, _))
          open = Some((c, n))
        end
        marks = marks.append(f != None)
      Some((oc, on)):
        if f is Some((fc, fl, rest)) and fc == oc and fl >= on and rest.empty?
          open = None
        end
        marks = marks.append(true)
    end
  end
  marks
end

## The text of a heading line, without its closing #s.
fn heading(line: Text) : Option(Text)
  examples
    heading("## Getting started") == Some("Getting started")
    heading("#hashtag") == None
    heading("####### seven") == None
    heading("# Title ##") == Some("Title")
  end

  cs = line.chars
  n = run_length(cs, 0, '#')
  return None if n < 1 or n > 6 or cs.get(n) != Some(' ')
  text = Text.from_chars(cs.drop(n + 1)).trim
  rc = text.chars.reverse
  k = run_length(rc, 0, '#')
  if k > 0 and (k == rc.size or rc.get(k) == Some(' '))
    Some(Text.from_chars(rc.drop(k).reverse).trim)
  else
    Some(text)
  end
end

## A heading's text without code marks and link brackets and targets.
fn strip_markup(text: Text) : Text
  examples
    strip_markup("The `mo` command") == "The mo command"
    strip_markup("See [the guide](a.md) now") == "See the guide now"
  end

  cs = text.chars
  var out = []
  var i = 0
  while i < cs.size
    c = cs.get(i) or ' '
    if c == '`' or c == '[' or (c == '!' and cs.get(i + 1) == Some('['))
      i += 1
    else if c == ']'
      i += 1
      if cs.get(i) == Some('(')
        var depth = 0
        while i < cs.size
          d = cs.get(i) or ' '
          i += 1
          if d == '('
            depth += 1
          else if d == ')'
            depth -= 1
            break if depth == 0
          end
        end
      end
    else
      out = out.append(c)
      i += 1
    end
  end
  Text.from_chars(out)
end

## The anchor GitHub makes for a heading's text.
fn anchor(text: Text) : Text
  examples
    anchor("Getting started") == "getting-started"
    anchor("The `mo` command") == "the-mo-command"
    anchor("See [the guide](guide.md)!") == "see-the-guide"
    anchor("C++ & Rust_2") == "c--rust_2"
  end

  lower = Unicode16.lowercase(strip_markup(text))
  kept = lower.chars.filter(fn(c)
    c.ascii_letter? or c.ascii_digit? or c == ' ' or c == '-' or c == '_' or c.code > 127
  end)
  Text.from_chars(kept.map(fn(c) if c == ' ': '-' else: c end))
end

## The anchors of every heading in a Markdown text, in order.
fn anchors(text: Text) : List(Text)
  examples
    anchors("# A\n## A\n```\n# A\n```\n# A") == ["a", "a-1", "a-2"]
  end

  lines = text.lines
  marks = code_lines(lines)
  var seen = {}
  var result = []
  for (i, line) in lines.enumerate
    continue if marks.get(i) or false
    if heading(line) is Some(h)
      base = anchor(h)
      k = seen.get(base) or 0
      result = result.append(if k == 0: base else: "#{base}-#{k}")
      seen = seen.set(base, k + 1)
    end
  end
  result
end

## The target of a reference definition line.
fn reference_target(line: Text) : Option(Text)
  examples
    reference_target("[guide]: docs/guide.md") == Some("docs/guide.md")
    reference_target("   [a b]: <my file.md> \"Title\"") == Some("my file.md")
    reference_target("    [x]: y.md") == None
    reference_target("[^1]: a note") == None
    reference_target("[x] : y") == None
    reference_target("[x]: y.md \"T\"") == Some("y.md")
  end

  cs = line.chars
  indent = run_length(cs, 0, ' ')
  return None if indent > 3 or cs.get(indent) != Some('[') or cs.get(indent + 1) == Some('^')
  var i = indent + 1
  while i < cs.size and cs.get(i) != Some(']')
    i += if cs.get(i) == Some('\\'): 2 else: 1
  end
  return None if i == indent + 1 or cs.get(i + 1) != Some(':')
  rest = Text.from_chars(cs.drop(i + 2)).trim
  return None if rest.empty?
  if rest.starts_with?("<")
    inner = rest.chars.drop(1)
    close = run_until(inner, '>')
    return None if close == inner.size
    Some(Text.from_chars(inner.take(close)))
  else
    rest.words.first
  end
end

## The position of the first c in cs, or cs's size.
fn run_until(cs: List(Char), c: Char) : Int
  var i = 0
  while i < cs.size and cs.get(i) != Some(c)
    i += 1
  end
  i
end

fn skip_space(cs: List(Char), from: Int) : Int
  var i = from
  while cs.get(i) == Some(' ') or cs.get(i) == Some('\t') or cs.get(i) == Some('\n')
    i += 1
  end
  i
end

## The start of a run of exactly n backticks at or after from.
fn closing_run(cs: List(Char), from: Int, n: Int) : Option(Int)
  var j = from
  while j < cs.size
    if cs.get(j) == Some('`')
      k = run_length(cs, j, '`')
      return Some(j) if k == n
      j += k
    else
      j += 1
    end
  end
  None
end

## Reads a link's destination after its "(": where the target starts, where the link ends, and the
## target.
fn destination(cs: List(Char), from: Int) : Option((Int, Int, Text))
  var i = skip_space(cs, from)
  var start = i
  var target = []
  if cs.get(i) == Some('<')
    start = i + 1
    i += 1
    while cs.get(i) != Some('>')
      a = cs.get(i) or '\n'
      return None if a == '\n' or a == '<'
      target = target.append(a)
      i += 1
    end
    i += 1
  else
    var depth = 0
    while i < cs.size
      b = cs.get(i) or ' '
      break if b == ' ' or b == '\t' or b == '\n'
      break if b == ')' and depth == 0
      if b == '\\' and i + 1 < cs.size
        target = target.append(cs.get(i + 1) or b)
        i += 2
      else
        if b == '('
          depth += 1
        else if b == ')'
          depth -= 1
        end
        target = target.append(b)
        i += 1
      end
    end
  end
  i = skip_space(cs, i)
  q = cs.get(i) or ' '
  if q == '"' or q == '\'' or q == '('
    close = if q == '(': ')' else: q
    i += 1
    while i < cs.size and cs.get(i) != Some(close)
      i += if cs.get(i) == Some('\\'): 2 else: 1
    end
    return None if i >= cs.size
    i = skip_space(cs, i + 1)
  end
  return None if cs.get(i) != Some(')')
  Some((start, i + 1, Text.from_chars(target)))
end

## The inline links and images in some lines of Markdown, outside code spans.
fn inline_links(lines: List(Text)) : List(Ref)
  examples
    inline_links(["See [a](a.md) and ![b](<b c.png> \"B\")"]) ==
      [Ref(line: 1, target: "a.md"), Ref(line: 1, target: "b c.png")]
    inline_links(["`[a](a.md)` [b", "c](", "d.md)"]) == [Ref(line: 3, target: "d.md")]
    inline_links(["[![i](i.png)](p.md)"]) ==
      [Ref(line: 1, target: "i.png"), Ref(line: 1, target: "p.md")]
    inline_links(["[a] (b.md) \\[c](d.md) ``x`` [e](f(g).md)"]) == [Ref(line: 1, target: "f(g).md")]
  end

  var cs = []
  var line_of = []
  for (n, line) in lines.enumerate
    for c in line.chars
      cs = cs.append(c)
      line_of = line_of.append(n + 1)
    end
    cs = cs.append('\n')
    line_of = line_of.append(n + 1)
  end
  var links = []
  var open = 0
  var i = 0
  while i < cs.size
    c = cs.get(i) or ' '
    if c == '\\'
      i += 2
    else if c == '`'
      n = run_length(cs, i, '`')
      i = case closing_run(cs, i + n, n)
        Some(j): j + n
        None: i + n
      end
    else if c == '['
      open += 1
      i += 1
    else if c == ']' and open > 0
      open -= 1
      i += 1
      if cs.get(i) == Some('(') and destination(cs, i + 1) is Some((start, after, target))
        links = links.append(Ref(line: line_of.get(start) or 0, target: target))
        i = after
      end
    else if c == '\n' and cs.get(i + 1) == Some('\n')
      open = 0
      i += 1
    else
      i += 1
    end
  end
  links
end

## Every link in a Markdown text, in order of line.
fn links(text: Text) : List(Ref)
  examples
    links("[r]: r.md\n```\n[a](a.md)\n```\n[b](b.md)") ==
      [Ref(line: 1, target: "r.md"), Ref(line: 5, target: "b.md")]
  end

  lines = text.lines
  marks = code_lines(lines)
  var refs = []
  var kept = []
  for (i, line) in lines.enumerate
    if marks.get(i) or false
      kept = kept.append("")
    else if reference_target(line) is Some(t)
      refs = refs.append(Ref(line: i + 1, target: t))
      kept = kept.append("")
    else
      kept = kept.append(line)
    end
  end
  refs.concat(inline_links(kept)).sort_by(fn(l) l.line end)
end

## Whether a target starts with a scheme, such as https:.
fn scheme?(target: Text) : Bool
  examples
    scheme?("https://example.com")
    scheme?("mailto:a@b.c")
    not scheme?("docs/a:b.md")
    not scheme?("a.md")
    not scheme?(":x")
  end

  cs = target.chars
  n = cs.size
  var i = 0
  while i < n and (cs.get(i) or ' ').ascii_letter?
    i += 1
  end
  i > 0 and cs.get(i) == Some(':')
end

## The path from FOLDER that a link's path leads to, or None when it leaves FOLDER.
fn resolve(from: Text, path: Text) : Option(Text)
  examples
    resolve("README.md", "docs/guide.md") == Some("docs/guide.md")
    resolve("docs/a.md", "../b.md") == Some("b.md")
    resolve("docs/a.md", "/b.md") == Some("b.md")
    resolve("docs/a.md", "../../b.md") == None
    resolve("a.md", "./") == Some(".")
  end

  parts = from.split("/")
  var segments = if path.starts_with?("/"): [] else: parts.take(parts.size - 1)
  for segment in path.split("/")
    continue if segment.empty? or segment == "."
    if segment == ".."
      return None if segments.empty?
      segments = segments.take(segments.size - 1)
    else
      segments = segments.append(segment)
    end
  end
  Some(if segments.empty?: "." else: segments.join("/"))
end

## Why a link from the file at from to target is broken, if it is.
fn problem(root: ReadFiles, from: Text, target: Text, known: Map(Text, List(Text))) : Option(Text)
  parts = target.split("#")
  path = (parts.first or "").replace("%20", " ")
  fragment = parts.drop(1).join("#")
  if path.empty?
    found = fragment.empty? or (known.get(from) or []).contains?(fragment)
    return if found: None else: Some("no such heading")
  end
  resolved = case resolve(from, path)
    Some(p): p
    None: return Some("leaves the folder")
  end
  return Some("no such file") if not root.exists?(resolved)
  return None if fragment.empty? or not resolved.ends_with?(".md")
  heads = case known.get(resolved)
    Some(hs): hs
    None:
      case root.read_text(resolved)
        Ok(text): anchors(text)
        Error(_): return None
      end
  end
  if heads.contains?(fragment): None else: Some("no such heading")
end

fn reason(error: FileError) : Text
  case error
    NotFound(path: _): "no such folder"
    NotAllowed(path: _): "not allowed"
    NotText(path: _): "not text"
    Failed(path: _, reason: r): r
  end
end

## Whether a path from FOLDER names a Markdown file outside folders whose names start with ".".
fn markdown?(path: Text) : Bool
  examples
    markdown?("docs/a.md")
    not markdown?(".git/a.md")
    not markdown?("a/.hidden/b.md")
    markdown?(".a.md")
    not markdown?("a.txt")
  end

  parts = path.split("/")
  path.ends_with?(".md") and not parts.take(parts.size - 1).any?(fn(p) p.starts_with?(".") end)
end

fn check(folder: Text, console: Console, files: ReadFiles) : Int
  root = files.within(folder)
  entries = case root.walk(".")
    Ok(es): es
    Error(e):
      console.error("linkcheck: #{folder}: #{reason(e)}")
      return 2
  end
  paths = entries.filter(fn(e) e.kind == File and markdown?(e.name) end).map(fn(e) e.name end)
  var texts = {}
  for path in paths
    case root.read_text(path)
      Ok(text): texts = texts.set(path, text)
      Error(e):
        console.error("linkcheck: #{folder}/#{path}: #{reason(e)}")
        return 2
    end
  end
  var known = {}
  for (path, text) in texts
    known = known.set(path, anchors(text))
  end
  var checked = 0
  var broken = []
  for (path, text) in texts
    for link in links(text)
      continue if scheme?(link.target)
      checked += 1
      if problem(root, path, link.target, known) is Some(r)
        broken = broken.append(Broken(path: path, line: link.line, target: link.target, reason: r))
      end
    end
  end
  for b in broken.sort_by(fn(b) (b.path, b.line) end)
    console.print("#{b.path}:#{b.line}: #{b.target}: #{b.reason}")
  end
  if broken.empty?
    console.print("ok: #{checked} links in #{paths.size} files")
    0
  else
    console.print("#{broken.size} broken links")
    1
  end
end

fn main(args: Args, console: Console, files: ReadFiles) : Int
  case args.list
    []: check(".", console, files)
    [folder] if not folder.starts_with?("-"): check(folder, console, files)
    _:
      console.error("usage: linkcheck [FOLDER]")
      2
  end
end

test "the example from the task"
  console = Console.fake()
  files = Files.in_memory({
    "README.md": "See [the guide](docs/guide.md#getting-started), [missing](docs/old.md)\nand [the site](https://example.com).\n",
    "docs/guide.md": "# Guide\n\n## Getting started\n"
  }).read_only
  assert main(Args.of([]), console, files) == 1
  assert console.printed == ["README.md:1: docs/old.md: no such file", "1 broken links"]
end
