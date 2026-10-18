module Main

fn main(console: Console)
  usage = """
    Usage: report [--json] FILE
      indented line
    Prints a summary.
    """
  console.print(usage)
  console.print("#{usage.lines.size}")
  console.print("""
    inner #{1 + 1}
    """)
  console.print("""
    a
    b
    """.replace("\n", "+"))
end
