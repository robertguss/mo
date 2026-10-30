module Main

enum Choice
  First(Text)
  Second(Text)
end

enum Envelope
  Rejected(Int)
  Notification(params: Choice)
  Message(id: Text, name: Text, params: Choice)
end

fn pick(outer: Map(Text, Text), inner: Choice) : Envelope
  name = case outer.get("name")
    Some(text): text
    None: return Rejected(0)
  end
  value = case outer.get("id")
    Some(text): text
    None:
      case inner
        First(_): return Rejected(1)
        Second(_): return Notification(params: inner)
      end
  end
  Message(id: value, name: name, params: inner)
end

fn main() : Int
  assert pick({}, First("discard")) == Rejected(0)
  assert pick({"name": "n"}, First("discard")) == Rejected(1)
  assert pick({"name": "n"}, Second("kept")) == Notification(params: Second("kept"))
  assert pick({"name": "n", "id": "i"}, Second("kept")) == Message(
    id: "i", name: "n", params: Second("kept")
  )
  17
end
