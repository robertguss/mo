module Main

struct Box
  value: Int
  apply: fn(Int) : Int
end

struct Packet
  value: Int
end

fn value(_box: Box) : Int
  999
end

fn value(packet: Packet) : Int
  packet.value + 1
end

fn defaults() : Int
  7
end

fn report(console: Console, text: Text)
  console.print(text)
end

fn main(console: Console)
  box = Box(value: 3, apply: fn(n) n * 2 end)
  console.print("#{box.value} #{value(box)} #{box.apply(5)}")
  console.print("#{value(Packet(value: 4))} #{defaults()}")
  var count = 1
  count += 2
  read = fn() count end
  console.print("#{read()}")
  send = fn(out, text) report(out, text) end
  send(console, "explicit capability")
  take = fn(n)
    return 11 if n < 0
    n + 1
  end
  console.print("#{take(-1)} #{take(2)} still in main")
  maybe = fn(text)
    n = try text.to_int
    Some(n + 1)
  end
  console.print("#{maybe("bad")} #{maybe("4")} still in main")
end
