module Main

enum AppError
  ItemMissing(Int)
  Forbidden
end

api
  get "/items/:id" read_item
  status ItemMissing(_) = 404
end

fn read_item(id: Int) : Result(Int, AppError)
  return Error(Forbidden) if id < 0
  return Error(ItemMissing(id)) if id == 0
  Ok(id)
end
