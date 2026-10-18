module Shop.Catalog
expose describe, price

fn price(item: Text) : Int
  case item
    "pen": 3
    "ink": 5
    _: 0
  end
end

fn describe(item: Text) : Text
  "#{item} costs #{price(item)}"
end
