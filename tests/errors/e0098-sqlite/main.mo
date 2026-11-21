module Main

fn main(console: Console)
  sqlite = Sqlite.fake()
  if sqlite.open("test.db") is Ok(_)
    console.print("opened")
  end
end
