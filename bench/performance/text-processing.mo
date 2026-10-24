module TextProcessing

fn main(console: Console)
  text = "  Alpha,beta,λ  \n".repeat(10_000)
  var words = 0
  var bytes = 0
  for line in text.lines
    cleaned = line.trim.ascii_lowercase.replace(",", " ")
    words += cleaned.words.size
    bytes += cleaned.byte_count
  end
  console.print("#{words} #{bytes}")
end
