module Main

fn parse(text: Text) : Int
  try
    text.to_int
  catch problem
    0
  end
end
