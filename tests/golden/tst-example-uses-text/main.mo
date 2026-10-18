module Main

fn shout(text: Text) : Text
  examples
    shout("hi") == "HI!"
    shout("") == "!"
    shout("ünï") == "üNï!"
  end

  "#{text.ascii_uppercase}!"
end
