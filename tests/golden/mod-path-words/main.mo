module Main
use Words.HTTP2Server
use Words.OrderBook
use Words.Unicode16Tool
use Words.XMLReader

fn main(console: Console)
  console.print(XMLReader.name())
  console.print(HTTP2Server.name())
  console.print(Unicode16Tool.name())
  console.print(OrderBook.name())
end
