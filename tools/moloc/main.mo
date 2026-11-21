## `moloc` — fast polyglot source, comment, and blank line counter (`tokei`/`scc` in Mo)
## built on `ReadFiles.walk`, multi-line block comment tracking, and `Unicode16` widths.
##
##   mo run tools/moloc/main.mo [PATH] [--json]
module Main
use Json
use Unicode16

struct CommentSyntax
  line_prefixes: List(Text)
  block_delims: Option((Text, Text))
end

struct FileCounts
  lines: Int
  blank: Int
  comment: Int
  code: Int
end

struct LangSummary
  language: Text
  files: Int
  lines: Int
  blank: Int
  comment: Int
  code: Int
end

enum MolocCommand
  Scan(target: Text, json?: Bool)
end

fn detect_language(path: Text) : Option((Text, CommentSyntax))
  examples
    detect_language("src/main.mo") is Some(("Mo", _))
    detect_language("src/vm/execute.c") is Some(("C", _))
    detect_language("formal/Core.lean") is Some(("Lean", _))
    detect_language("logo.png") == None
  end

  base = path.split("/").last or path
  if base == "Makefile"
    return Some(("Makefile", CommentSyntax(line_prefixes: ["#"], block_delims: None)))
  end
  return None if not base.contains?(".")
  ext = (base.split(".").last or "").ascii_lowercase
  c_like = CommentSyntax(line_prefixes: ["//"], block_delims: Some(("/*", "*/")))
  hash_like = CommentSyntax(line_prefixes: ["#"], block_delims: None)
  case ext
    "mo": Some(("Mo", hash_like))
    "c" | "h": Some(("C", c_like))
    "cc" | "cpp" | "hpp": Some(("C++", c_like))
    "rs": Some(("Rust", c_like))
    "go": Some(("Go", c_like))
    "ts" | "tsx": Some(("TypeScript", c_like))
    "js" | "jsx": Some(("JavaScript", c_like))
    "py": Some(("Python", hash_like))
    "sh": Some(("Shell", hash_like))
    "lean": Some(("Lean", CommentSyntax(line_prefixes: ["--"], block_delims: Some(("/-", "-/")))))
    "sql": Some(("SQL", CommentSyntax(line_prefixes: ["--"], block_delims: Some(("/*", "*/")))))
    "md": Some(("Markdown", CommentSyntax(line_prefixes: [], block_delims: Some(("<!--", "-->")))))
    "json": Some(("JSON", CommentSyntax(line_prefixes: [], block_delims: None)))
    _: None
  end
end

fn ignored_path?(path: Text) : Bool
  examples
    ignored_path?(".git/config")
    ignored_path?("build/gcc/debug/mo")
    ignored_path?("src/node_modules/pkg/index.js")
    not ignored_path?("src/check/apis.c")
  end

  segs = path.split("/")
  segs.any?(fn(seg)
    seg == ".git" or
      seg == "build" or
      seg == "node_modules" or
      seg == "target" or
      seg == ".lake" or
      seg == "vendor"
  end)
end

fn starts_with_line_comment?(trimmed: Text, prefixes: List(Text)) : Bool
  prefixes.any?(fn(p) not p.empty? and trimmed.starts_with?(p) end)
end

## Counts blank, comment, and code lines in one file, tracking multi-line block comments.
fn count_text(content: Text, syntax: CommentSyntax) : FileCounts
  promise
    result.blank + result.comment + result.code == result.lines
    result.blank >= 0 and result.comment >= 0 and result.code >= 0
  end

  examples
    count_text(
      "## doc\nfn add(a: Int) : Int\n  a + 1\nend\n",
      CommentSyntax(line_prefixes: ["#"], block_delims: None)
    ) ==
      FileCounts(lines: 4, blank: 0, comment: 1, code: 3)
    count_text(
      "/* a\n   b */\nx = 1;\n\n",
      CommentSyntax(line_prefixes: ["//"], block_delims: Some(("/*", "*/")))
    ) ==
      FileCounts(lines: 4, blank: 1, comment: 2, code: 1)
  end

  delims = case syntax.block_delims
    Some((o, c)) if not o.empty? and not c.empty?: Some((o, c))
    Some(_) | None: None
  end
  all_lines = content.lines
  var blank = 0
  var comment = 0
  var code = 0
  var in_block = false

  for raw_line in all_lines
    trimmed = raw_line.trim
    if trimmed.empty?
      blank += 1
    else if in_block
      comment += 1
      case delims
        Some((_, close_tok)):
          if trimmed.contains?(close_tok)
            after = trimmed.split(close_tok).last or ""
            in_block = false
            if not after.trim.empty? and
              not starts_with_line_comment?(after.trim, syntax.line_prefixes)
              comment -= 1
              code += 1
            end
          end
        None: in_block = false
      end
    else if starts_with_line_comment?(trimmed, syntax.line_prefixes)
      comment += 1
    else
      case delims
        Some((open_tok, close_tok)) if trimmed.starts_with?(open_tok):
          rest = trimmed.drop_prefix(open_tok) or ""
          if rest.contains?(close_tok)
            after = rest.split(close_tok).last or ""
            if after.trim.empty? or starts_with_line_comment?(after.trim, syntax.line_prefixes)
              comment += 1
            else
              code += 1
            end
          else
            in_block = true
            comment += 1
          end
        Some((open_tok, close_tok)):
          code += 1
          if trimmed.contains?(open_tok)
            tail = trimmed.split(open_tok).last or ""
            if not tail.contains?(close_tok)
              in_block = true
            end
          end
        None: code += 1
      end
    end
  end

  FileCounts(lines: all_lines.size, blank: blank, comment: comment, code: code)
end

fn pad_display_right(text: Text, width: Int) : Text
  w = Unicode16.display_width(text)
  if w >= width: text else: "#{text}#{" ".repeat(width - w)}"
end

fn pad_int_left(n: Int, width: Int) : Text
  n.to_text.pad_left(width)
end

fn render_table(rows: List(LangSummary)) : Text
  rule = "----------------------------------------------------------------------"
  header = "#{pad_display_right("Language", 16)} #{pad_int_left_str("Files", 8)} #{pad_int_left_str(
    "Lines",
    10
  )} #{pad_int_left_str("Code", 10)} #{pad_int_left_str("Comments", 10)} #{pad_int_left_str(
    "Blanks",
    10
  )}"
  body = rows.map(fn(r)
    "#{pad_display_right(r.language, 16)} #{pad_int_left(r.files, 8)} #{pad_int_left(r.lines, 10)} #{pad_int_left(
      r.code,
      10
    )} #{pad_int_left(r.comment, 10)} #{pad_int_left(r.blank, 10)}"
  end)
  tot_files = rows.map(fn(r) r.files end).sum
  tot_lines = rows.map(fn(r) r.lines end).sum
  tot_code = rows.map(fn(r) r.code end).sum
  tot_comment = rows.map(fn(r) r.comment end).sum
  tot_blank = rows.map(fn(r) r.blank end).sum
  total_line = "#{pad_display_right("Total", 16)} #{pad_int_left(tot_files, 8)} #{pad_int_left(
    tot_lines,
    10
  )} #{pad_int_left(tot_code, 10)} #{pad_int_left(tot_comment, 10)} #{pad_int_left(tot_blank, 10)}"
  [rule, header, rule].concat(body).concat([rule, total_line, rule]).join("\n")
end

fn pad_int_left_str(label: Text, width: Int) : Text
  label.pad_left(width)
end

fn scan_path(files: ReadFiles, target: Text) : Result(List(LangSummary), FileError)
  entries = if files.exists?(target)
    case files.walk(target)
      Ok(walked):
        prefix = if target == ".": "" else: "#{target}/"
        walked.filter_map(fn(e)
          if e.kind == File: Some("#{prefix}#{e.name}") else: None
        end)
      Error(Failed(path: _, reason: "not a folder")): [target]
      Error(e): return Error(e)
    end
  else
    return Error(NotFound(path: target))
  end

  var by_lang = {}
  for path in entries
    continue if ignored_path?(path)
    case detect_language(path)
      Some((lang, syntax)):
        case files.read_text(path)
          Ok(text):
            c = count_text(text, syntax)
            prev = by_lang.get(lang) or
              LangSummary(language: lang, files: 0, lines: 0, blank: 0, comment: 0, code: 0)
            by_lang = by_lang.set(
              lang,
              LangSummary(
                language: lang,
                files: prev.files + 1,
                lines: prev.lines + c.lines,
                blank: prev.blank + c.blank,
                comment: prev.comment + c.comment,
                code: prev.code + c.code
              )
            )
          Error(NotText(path: _)): Unit
          Error(e): return Error(e)
        end
      None: Unit
    end
  end

  Ok(by_lang.values.sort_by_desc(fn(r) r.code end))
end

fn parse_cli(args: List(Text)) : Option(MolocCommand)
  case args
    []: None
    ["--json"]: Some(Scan(target: ".", json?: true))
    [target] if not target.starts_with?("-"): Some(Scan(target: target, json?: false))
    [target, "--json"] if not target.starts_with?("-"): Some(Scan(target: target, json?: true))
    ["--json", target] if not target.starts_with?("-"): Some(Scan(target: target, json?: true))
    _: None
  end
end

fn run_cli(args: Args, files: ReadFiles, io: Console) : Int
  cmd = case parse_cli(args.list)
    Some(c): c
    None:
      io.error("usage: moloc PATH [--json]")
      return 2
  end
  case cmd
    Scan(target: target, json?: json?):
      case scan_path(files, target)
        Ok(rows):
          if json?
            io.print(Json.encode_pretty(rows))
          else
            io.print(render_table(rows))
          end
          0
        Error(e):
          io.error("moloc: #{e}")
          1
      end
  end
end

fn main(args: Args, files: ReadFiles, io: Console) : Int
  run_cli(args, files, io)
end

test "polyglot directory walk, block comments, ignored folders, and JSON/table reports"
  fs = Files.in_memory({
    "src/main.mo": "## Main entry\nmodule Main\n\nfn main()\n  x = 1\nend\n",
    "src/vm.c": "/* VM loop\n * fast path\n */\nint run(void) {\n    // step\n    return 0;\n}\n",
    "formal/Core.lean": "/- theorem -/\ndef ok : Nat := 1\n",
    "build/generated.c": "int ignored(void) { return 1; }\n"
  }).read_only

  io_tbl = Console.fake()
  assert run_cli(Args.of(["."]), fs, io_tbl) == 0
  tbl = io_tbl.printed.first or ""
  assert tbl.contains?("Mo")
  assert tbl.contains?("C")
  assert tbl.contains?("Lean")
  assert tbl.contains?("Total")

  io_json = Console.fake()
  assert run_cli(Args.of([".", "--json"]), fs, io_json) == 0
  assert Json.decode[List(LangSummary)](io_json.printed.first or "") ==
    Ok([
      LangSummary(language: "Mo", files: 1, lines: 6, blank: 1, comment: 1, code: 4),
      LangSummary(language: "C", files: 1, lines: 7, blank: 0, comment: 4, code: 3),
      LangSummary(language: "Lean", files: 1, lines: 2, blank: 0, comment: 1, code: 1)
    ])
end

test "empty CLI arguments exit 2"
  io = Console.fake()
  assert run_cli(Args.of([]), Files.in_memory({}).read_only, io) == 2
end
