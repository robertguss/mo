## Syntax accepts api server clauses in any order for the checker.
api
  port 8080
  cors "*"
  sqlite db from "app.db"
  auth Principal via authenticate
  get "/health" health
  post "/users" create_user
  get "/users/:id" get_user
  put "/users/:id" replace_user
  patch "/users/:id" update_user
  delete "/users/:id" delete_user
  status Unauthorized = 401
  status UserMissing(_) = 404
  status Conflict(_, _) = 409
end
