module Main
use Shop.Account
use Box
use Json
use Tag

fn richest(accounts: List(Account)) : Text
  accounts.map(Account.describe).join(", ")
end

fn unbox(box: Box(Int)) : Int
  box.item
end

fn kind(value: Json) : Text
  case value
    Json.Null: "null"
    Json.Boolean(_): "boolean"
    Json.Number(_): "number"
    Json.String(_): "string"
    Json.Array(_): "array"
    Json.Object(_): "object"
  end
end

fn tagged(tag: Option(Tag)) : Bool
  tag is Some(_)
end

fn main(console: Console)
  console.print(richest([Account.open("Ada", 10), Account.open("Bo", 0)]))
  console.print("#{unbox(Box.of(7))}")
  console.print(kind(Json.Null))
  console.print("#{tagged(None)}")
end
