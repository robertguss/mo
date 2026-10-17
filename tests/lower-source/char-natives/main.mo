module Main

fn main(console: Console) : Int
  digits = "a1-9 Z".chars.map(fn(c) c.ascii_digit? end)
  letters = "a1-9 Z".chars.map(fn(c) c.ascii_letter? end)
  spaces = "a 1".chars.map(fn(c) c.ascii_white_space? end)
  qualified = Char.ascii_digit?('7')
  case 66.to_char
    Some(c): console.print("#{digits}/#{letters}/#{spaces}/#{qualified}/#{c.code}/#{c.ascii_lowercase}/#{'q'.ascii_uppercase}")
    None: return 1
  end
  fields = {"a": 1, "b": 2}
  nothing = {"a": 1}.remove("a")
  console.print("#{fields.has?("a")}/#{fields.has?("z")}/#{nothing.empty?}/#{fields.empty?}/#{fields.keys}/#{fields.entries}")
  0
end
