module Main

api
  get "/users/:id" first_user
  get "/users/:uid" second_user
end

fn first_user(id: Int) : Int
  id
end

fn second_user(uid: Int) : Int
  uid
end
